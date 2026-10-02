# full-orderbook-lab

比較不同 L3（market-by-order）order book 資料結構的實驗場。

## 資料來源

| 交易所 / feed | 公開樣本 | 與 HKEX OMD-C 的相似度 |
|---|---|---|
| **NYSE XDP Integrated Feed（採用）** | ✅ `ftp.nyse.com/Real Time Data Samples/NYSE XDP/` 有整小時 PCAP | 最高。OMD 平台由 NYSE Technologies 建置，封包/訊息框架同源 |
| Nasdaq TotalView-ITCH 5.0 | ✅ `emi.nasdaq.com/ITCH/`（整天，數 GB） | 中：訊息語意相近，但框架（MoldUDP64）不同 |
| HKEX OMD-C Securities FullTick | ❌ canned PCAP 僅發給直連客戶；Historical Full Book 付費 | — |

XDP 與 OMD-C 對照：

| | HKEX OMD-C (SF) | NYSE XDP Integrated |
|---|---|---|
| 封包標頭 | PktSize, MsgCount, SeqNum, SendTime | PktSize, DeliveryFlag, NumberMsgs, SeqNum, SendTime(s+ns)，16 bytes |
| 訊息標頭 | MsgSize u16, MsgType u16 | MsgSize u16, MsgType u16 |
| Add / Modify / Delete | 30 / 31 / 32 | 100 / 101 / 102 |
| 成交 | Trade (50) | Order Execution (103)，另有 Replace (104) |
| 識別 | SecurityCode + OrderId（per security 唯一） | SymbolIndex + OrderID |
| Byte order | little-endian | little-endian |

XDP 訊息 layout 取自 [Open Markets Initiative 的 Wireshark dissector](https://github.com/Open-Markets-Initiative/wireshark-lua)
（`Nyse_NyseEquities_IntegratedFeed_Pillar_v2_5_h`），見 `include/obl/xdp/messages.hpp`。
拿到真實樣本後先跑 `obl_dump`：若 short msgs / unknown-order refs 異常，代表版本或 layout 不符。

## 架構

```
pcap (Ethernet/VLAN/SLL/raw IP, µs/ns, .gz)
  └─ xdp::process_datagram      序號追蹤 + A/B 線仲裁（gap / duplicate / 部分重疊 / reset）
       └─ Visitor (static dispatch)
            ├─ xdp::BookAdapter     → L3Book（end-to-end）
            └─ xdp::EventRecorder   → feed 中立的 book::Event（只量資料結構）
L3Book<Levels, Index>
  ├─ OrderPool   固定大小 Order 節點（32-bit index）＋每個價位的 intrusive FIFO 雙向鏈結
  ├─ Index       (symbol, order id) → pool index
  └─ Levels<Side> 每邊的價位容器
```

Feed adapter 只把線上訊息轉成 `add / modify / execute / remove / replace / clear_symbol`，
之後加 OMD-C adapter 時 book 端不需改動（key 已含 symbol，因為 OMD-C 的 OrderId 只在單一證券內唯一）。

### 目前的變體（`include/obl/book/variants.hpp`）

矩陣設計：每種 Levels 搭 open addressing（隔離價位容器的影響）、每種 Index 搭 dense array（隔離 index 的影響）。

| Levels | 說明 |
|---|---|
| `std::map` | 紅黑樹，baseline |
| `std::map(pool alloc)` | 同上，node 改由固定大小 free-list arena 配置（`node_pool.hpp`） |
| `absl::btree_map` | B-tree，每個 node 多個 key，查找碰到的 cache line 少 |
| `sorted_vector(linear)` | 連續陣列，best 在尾端，從尾端線性搜尋 |
| `sorted_vector(binary)` | 同上，二分搜尋 |
| `sorted_vector_soa(linear)` | 價格獨立成一個陣列（SoA），搜尋只掃 8 bytes/level |
| `dense_array` | 以 tick 為索引的稠密陣列，O(1) 找價位；bitmap 找下一個 best；超出視窗/非整 tick 價格進 overflow map |

| Index | 說明 |
|---|---|
| `unordered_map` | std baseline（node-based） |
| `open_addressing` | 自寫：linear probing、2 的冪容量、load ≤ 0.5、backward-shift 刪除（無 tombstone） |
| `ankerl::unordered_dense` | robin-hood + dense value array |
| `absl::flat_hash_map` | Swiss table（SIMD control bytes） |

新增變體：寫一個符合 `OrderedMapLevels` 介面的 Levels（`get_or_create / update / best / for_each / size / clear / set_tick`）
或符合 `HashMapOrderIndex` 介面的 Index，加到 `variants.hpp` 的 `for_each_variant`，測試與 benchmark 會自動涵蓋。

### Software prefetch

`obl_bench --prefetch K`：handler 一次收到一個封包（多筆訊息），可以往前看。
兩階段：第 i+2K 筆事件先 prefetch index slot，第 i+K 筆時查 index（已在 cache）再 prefetch order 節點。
只有提供 `prefetch()` 的 index 會做第一階段（open addressing、absl）。

## 建置與使用

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build                     # 含與 naive reference book 的差分測試
# sanitizer：GCC 的 UBSan 編不過 abseil，sanitizer build 請關 abseil
cmake -S . -B build-asan -DOBL_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug -DOBL_WITH_ABSEIL=OFF -DOBL_WITH_MIMALLOC=OFF

# 合成資料（XDP 格式 PCAP）
./build/obl_gen --out data/synth_5m.pcap --messages 5e6 --symbols 300

# 檢查 capture（訊息統計、序號、book 一致性）
./build/obl_dump data/synth_5m.pcap --print 20

# Benchmark：每個變體在 fork 出的子行程跑（乾淨 heap、獨立量 RSS），結束時比對全深度 + 佇列順序 checksum
./build/obl_bench data/synth_5m.pcap --repeat 3 --latency [--reserve 1000000] [--only dense] [--prefetch 4]

# 不同 malloc（同一個 binary，LD_PRELOAD）× 有無 transparent huge pages
bench/run_allocators.sh data/synth_5m.pcap --repeat 5 --prefetch 4
```

CMake 會用 FetchContent 抓 abseil（20260526.0）、ankerl::unordered_dense（v5.2.0）、mimalloc（v3.5.4，編成
`build/lib/libmimalloc.so` 給 LD_PRELOAD 用）；可用 `-DOBL_WITH_ABSEIL=OFF` 等關掉。jemalloc 用系統的 `libjemalloc2`。

真實 NYSE 樣本：下載 `ftp.nyse.com/Real Time Data Samples/NYSE XDP/NYSE_IBF/` 的檔案放到 `data/`，
`obl_dump`/`obl_bench` 可直接讀 `.pcap.gz`。A/B 線預設以 UDP port 做仲裁（`--key-by-group` 改為 group+port）；
`--port P` 只看單一 channel。pcapng 需先 `editcap -F pcap` 轉檔。

## 合成資料模型（`include/obl/sim/generator.hpp`）

- 事件比例預設 add 45% / delete 38% / modify 7% / replace 5% / execution 5%
- 新單離對手 best 的距離 ~ geometric(0.3) tick，1% 落在 200–3200 tick 外（製造深而稀疏的尾巴）
- Symbol 活躍度 Zipf 分佈；每個 symbol 有穩態掛單量目標；價格對錨點均值回歸，不會交叉
- OrderID 不連續（模擬 OMD-C）；modify 有保留/失去排隊位置兩種（PositionChange）
- 生成器自帶 shadow book，測試會驗證所有訊息都引用存在的委託

## 結果（2026-10-01，合成資料）

5M 訊息、300 symbols、結束時約 30 萬筆掛單；4 vCPU Xeon @ 2.1GHz 雲端 VM。
**同一設定重跑差 10–15%**，以下只把超出雜訊的差異當結論。完整輸出：`results/2026-10-01_alloc_sweep_synth5m.txt`
（`--repeat 5 --prefetch 4`，6 種 allocator 設定 × 11 變體，所有變體 checksum 一致）。

glibc（無 THP）的 best-of-5，ns/event：

| variant | 一般 replay | prefetch k=4 | dRSS MB |
|---|---|---|---|
| std::map + unordered_map | 350 | 315 | 47 |
| std::map + open_addressing | 175 | 130 | 64 |
| std::map(pool alloc) + open_addressing | 174 | 143 | 53 |
| absl::btree_map + open_addressing | 165 | 124 | 55 |
| sorted_vector(linear) + open_addressing | 147 | 103 | 37 |
| sorted_vector(binary) + open_addressing | 175 | 112 | 37 |
| sorted_vector_soa(linear) + open_addressing | 150 | 103 | 74 |
| dense_array + open_addressing | 144 | 104 | 132 |
| dense_array + unordered_map | 338 | 292 | 121 |
| dense_array + ankerl::unordered_dense | 197 | 138 | 124 |
| dense_array + absl::flat_hash_map | 150 | 109 | 122 |

觀察：

1. **Order index 影響最大**：`std::unordered_map` 比 open addressing / absl 慢約 2 倍；
   ankerl 慢 20–30%（value 存在獨立 dense 陣列，erase 要搬最後一筆，多一次 cache miss）。
   自寫 open addressing 與 absl Swiss table 在雜訊內打平。
2. **Software prefetch 是這輪最大的單一改善**：k=4 在所有變體都快 25–30%，因為成本主要是 order/index 的 cache miss，
   而封包內多筆訊息讓 handler 本來就能往前看。
3. **價位容器之間差距小（~15%）**：線性搜尋的 sorted vector 與 dense array 最好；B-tree 比紅黑樹好約 5–10%；
   SoA 與一般 sorted vector 在這個資料（每邊約 50–100 個價位、活動集中在 best 附近）沒有可量測差異；
   二分搜尋比線性慢，因為要找的價位幾乎都在尾端附近。
4. **Allocator 幾乎沒差**：hot path（order pool + open addressing）本來就不 malloc，只有 map node 會配置；
   glibc / jemalloc / mimalloc 的差異都在雜訊內。pool allocator 對 std::map 也沒有可量測改善。
   記憶體方面 jemalloc 的 RSS 最低（vector 類 28 MB vs glibc 37 MB）。
5. **Transparent huge pages**：glibc + THP（`GLIBC_TUNABLES=glibc.malloc.hugetlb=1`，確認有 ~500 MB 落在 huge page）
   讓多數變體快 5–10%（TLB miss 減少），接近雜訊上緣；jemalloc/mimalloc 開 THP 無一致差異。
   （mimalloc 的 dRSS 出現負值是因為它會歸還前一輪釋放的記憶體，該欄不可直接比較。）

結論：目前最佳組合是 **dense_array 或 sorted_vector(linear) + open addressing + prefetch + THP**，約 95–100 ns/event。
下一步要再往下壓，重點在減少每筆事件的 cache miss 數（order 節點與 index 合併、per-symbol 小 index 等），而不是換 allocator。

## 結果 2：減少 cache miss（32B order 節點、compact index）

方法：把前一個 commit 編成 baseline，與新版**交錯**各跑兩次（A B A B，`--repeat 5 --prefetch 4`），
降低 VM 雜訊的影響。原始輸出：`results/2026-10-01_node32_compact_ab.txt`。

| 改動 | 無 prefetch | prefetch | 結論 |
|---|---|---|---|
| Order 節點 40B → 32B、32B 對齊（不跨 cache line） | 各變體 ±10% 兩個方向都有 | 同左 | **無可量測差異** |
| `compact_fp(8B)` index（fingerprint + pool index，用 order 節點驗證 key） | dense_array：**111–122 ns** vs open addressing 133–195 ns | 100–104 ns vs 92–108 ns | 無 prefetch 時穩定快 **15–20%**；有 prefetch 時打平 |
| sorted_vector(linear) + compact | 114–120 ns | 106–116 ns | RSS 最低（26 MB） |

Prefetch 距離掃描（dense_array）：k=1 只有部分效果；k=2–16 都在 92–107 ns，k=32 開始變差。

解讀：

- Compact index 把 index 從 16 MB 縮成 8 MB（每條 cache line 8 個 slot），沒 prefetch 時 index miss 變少，所以快。
  有 prefetch 時 index miss 已被隱藏，兩者收斂到同一個 ~95–105 ns 的底。
- 一開始 compact + prefetch 反而沒變快：第二階段 prefetch 呼叫 `find()`，而 compact index 驗證 key 要讀 order 節點，
  prefetch 自己就卡在它要預取的 miss 上。加了不驗證的 `peek()` 專給 prefetch 用之後才恢復。
- 剩下的底推測來自 FIFO 雙向鏈結：刪單時要改前後鄰居節點（`prev`/`next`），那是兩條沒被 prefetch 的隨機 cache line。

測試補強：random 差分測試改成每個 symbol 各自編 order id（同一個 id 同時存在多個 symbol，符合 OMD-C 語意），
並加了 4-bit fingerprint 的變體強制碰撞。原本的測試抓不到「compact index 只比 id 不比 symbol」這個植入的 bug，補強後抓得到。

## 結果 3：FIFO 佇列結構（intrusive list vs 每價位陣列）—— 負面結果

假設：刪單要改前後鄰居節點（兩條隨機 cache line），是剩下成本的主因。
做法：`L3Book` 新增第三個模板參數 `Queues`：`ListQueues`（原本的雙向鏈結）或 `VectorQueues`
（每個價位一個 pool index 陣列，刪單只寫 tombstone，tombstone 過半時壓縮並改寫存活 order 的位置）。
`obl_bench --latency` 新增**依事件類型**的延遲（扣掉 rdtsc 本身成本）。原始輸出：`results/2026-10-01_vector_queue.txt`（跑兩次）。

| dense_array + compact_fp | ns/event | remove（mean） | add（mean） |
|---|---|---|---|
| ListQueues | 117–139 | 194–209 | 111–115 |
| VectorQueues | 174–181 | 275–319 | 122–142 |

**假設不成立**：VectorQueues 全面較慢（所有搭配都是）。原因判斷：

1. 合成資料最熱的價位約有 3,400 筆掛單，`slots[pos]` 本身就是大陣列裡的一條隨機 cache line，和碰鄰居節點差不多貴。
2. 壓縮時要改寫每個存活 order 節點的位置（存活至少一半），攤提下來每次刪單約多一次節點 miss。
3. 依類型拆解：remove ≈ add + ~90 ns，大約正好一次 miss——也就是 order 節點本身。這是查到 order 後一定要讀的，
   prefetch 正是在藏這個 miss，所以 prefetch 後的 ~95–105 ns 已接近這個資料規模下的底。

保留 `VectorQueues` 作為對照。注意：真實行情最熱價位的掛單數可能遠少於合成資料，到時候結論可能不同，要用真實樣本重測。

## 結果 4：真實資料（NYSE XDP 2019-01-22 樣本）

資料：`ftp.nyse.com/Real Time Data Samples/NYSE XDP/NYSE_IBF/` 全部 21 個小時檔（channel 89–100、A 線，約 760 檔證券），
依序串接處理：1,417 萬筆訊息，0 gap、0 malformed、0 短訊息、0 unknown-order、收盤後 book 清空。
原始輸出：`results/2026-10-01_nyse_20190122_real.txt`（`--repeat 3 --prefetch 4 --latency`）。

```bash
./build/obl_dump  data/nyse_ibf/*.gz --profile 1000000   # book 形狀、crossed/locked、新單落點分佈
./build/obl_bench data/nyse_ibf/*.gz --repeat 3 --prefetch 4 --latency
```

**真實資料抓到的 bug**：這個版本的 feed 中，所有 Replace 的 side byte、所有 Modify 的 PositionChange/side 都是 0。
原本 decoder 把非 `'S'` 一律當 Buy，被 replace 的賣單變成買單，30–50 檔證券整天 crossed。
修正：Replace 沒有 side 時沿用原委託的 side；Modify 依交易所規則判斷優先權（價格不變且數量沒增加才保留）。修正後 0 crossed。

**真實 book 的形狀**（`--profile`）：同時掛單約 3–4.6k 筆、約 320 檔有掛單、每邊平均 3.5 個價位、
最佳價位掛單數中位數 1（p99 約 10）；新單 56% 掛在 best、23% 改善 best、12% 在 1 cent 外。
事件比例 add 47.7% / delete 47.4% / modify 4.0% / execution 0.5% / replace 0.4%。

| variant | ns/event | prefetch k=4 | dRSS MB |
|---|---|---|---|
| sorted_vector(linear) + open_addressing | **48.2** | 63.1 | 5.6 |
| sorted_vector(linear) + compact_fp | **48.5** | 61.5 | 5.4 |
| sorted_vector(linear) + compact_fp + vector_queue | 50.5 | 62.2 | 6 |
| sorted_vector_soa(linear) + open_addressing | 51.1 | 64.0 | 11.7 |
| absl::btree_map + open_addressing | 54.7 | 71.4 | 5.3 |
| sorted_vector(binary) + open_addressing | 59.3 | 70.2 | 5.6 |
| std::map(pool alloc) + open_addressing | 62.2 | 76.6 | 11.4 |
| std::map + open_addressing | 65.3 | 85.8 | 11.3 |
| dense_array + compact_fp | 81.6 | 99.3 | 589 |
| std::map + unordered_map | 87.5 | 91.3 | 11.6 |
| dense_array + open_addressing | 90.0 | 93.0 | 587 |

與合成資料的結論比較：

1. **整體快 2–4 倍**：真實 book 很小（數千筆掛單），幾乎全在 cache 裡，cache miss 不再是主角。
2. **名次翻轉**：sorted_vector(linear) 最好（每邊只有 ~3.5 個價位，線性掃描幾乎一步到位）；
   **dense_array 變最差之一、RSS 590 MB**——因為用 symbol mapping 的 MPV 當 tick：價格 scale 6、MPV=100 表示 $0.0001，
   但 $1 以上實際價格格點是 $0.01（100 倍），陣列變得很稀疏、視窗不停擴張，對 cache 很不友善。這是 tick 設定問題，不是結構本身。
3. **Prefetch 反而變慢 25–30%**：資料已在 cache，prefetch 只剩額外的 index 查找成本。
4. **Index 差異縮小**：compact 與 open addressing 打平（index 很小），`std::unordered_map` 仍明顯較慢。
5. **vector_queue 與 list 打平**：最佳價位通常只有 1 筆，佇列結構幾乎無影響。

結論：**資料結構的最佳選擇取決於 book 的形狀**。合成資料（深 book、30 萬筆掛單）偏向 cache-miss 主導，
真實樣本（淺 book、數千筆）偏向指令數主導。之後的實驗以真實資料為主，合成資料要先依上面的分佈校準。

## 結果 5：熱門商品真實資料（Nasdaq TotalView-ITCH 5.0，2025-12-08）

資料：`emi.nasdaq.com/ITCH/Nasdaq ITCH/S120825-v50.txt.gz`（8.8 GB，6.5 億筆訊息，0 筆長度不符）。
取當天委託訊息最多的 20 檔：QQQ、NVDA、SPY、GOOGL、TSLA、GOOG、IWM、DIA、SOXL、SQQQ、TQQQ、NVDL、NFLX、PLTR、IVV、
SOXX、VOO、FBTC、IBIT、AMD，共 9,519 萬筆事件；replay 後 0 unknown-order、0 crossed、收盤 book 清空。
原始輸出：`results/2025-12-08_itch_top20.txt`（`--repeat 2 --prefetch 4 --latency`）。

```bash
./build/obl_itch data/itch/S120825-v50.txt.gz scan --top 25
./build/obl_itch data/itch/S120825-v50.txt.gz extract --top 20 --out data/itch/top20.ev --profile 8000000
./build/obl_bench --events data/itch/top20.ev --repeat 2 --prefetch 4 --latency
```

Book 形狀：同時約 **54 萬筆**掛單、每邊平均約 **1,900 個價位**（最多 7,400）、best 價位掛單數中位數 3–4；
新單 45% 掛在 best、17% 在 10 分錢以外（3.4% 超過 2 美元）。事件：add 36.7% / delete 35.0% / **replace 25.7%** /
execute 2.2% / cancel 0.4%。

| variant | ns/event | prefetch k=4 | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| dense_array + ankerl::unordered_dense | **125.1** | 124.1 | 87 | 565 | 1250 |
| dense_array + compact_fp(8B) | 130.5 | 141.1 | 107 | 516 | 1740 |
| dense_array + absl::flat_hash_map | 134.4 | 129.7 | 118 | 527 | 1264 |
| dense_array + open_addressing | 139.8 | **117.0** | 125 | 546 | 2856 |
| dense_array + open_addressing + vector_queue | 170.6 | 132.3 | 136 | 653 | 3209 |
| absl::btree_map + open_addressing | 193.9 | 157.8 | 207 | 713 | 3042 |
| std::map(pool alloc) + open_addressing | 214.0 | 192.7 | 218 | 976 | 3655 |
| std::map + open_addressing | 227.7 | 189.1 | 224 | 978 | 3924 |
| sorted_vector(binary) + open_addressing | 236.8 | 193.7 | 218 | 946 | 3841 |
| sorted_vector(linear) + open_addressing | 325.9 | 252.3 | 220 | 1877 | 6360 |
| dense_array + unordered_map | 379.4 | 377.1 | 228 | 1497 | 5770 |
| std::map + unordered_map | 464.4 | 459.2 | 377 | 1875 | 8948 |

觀察：

1. **名次再次翻轉**：深 book（每邊上千價位）時 **dense_array 最好**（O(1) 找價位，tick 已改成一分錢），
   在淺的 NYSE 樣本上最好的 `sorted_vector(linear)` 在這裡掉到倒數（要線性掃過大量價位；replace 平均 683 ns，
   因為新價位常離 best 很遠）。二分搜尋版本好很多（237 ns），B-tree 是樹狀結構裡最好的（194 ns）。
2. **Index**：`ankerl::unordered_dense` 在這份資料最好（合成資料上反而最慢）；compact / absl / open addressing 差距在 10% 內；
   `std::unordered_map` 仍慢約 3 倍。
3. **Prefetch 在大資料上有效**：open addressing 搭配時快 15–25%（dense_array 140 → 117 ns，全場最快），
   但對 compact（驗證 key 時仍要讀節點）、ankerl（沒有 prefetch API）幾乎無效。
4. **vector_queue 仍然較慢**（171 vs 140 ns），結論與合成資料一致。
5. 尾端延遲：dense_array 系列 p99 約 520–650 ns；std 容器 p99 約 1–1.9 µs、p99.9 達 4–9 µs。

**三份資料的總結**：沒有單一最佳結構。

| 資料 | 每邊價位 | 最佳 | 次佳 |
|---|---|---|---|
| NYSE 2019 樣本（ETF 為主、淺） | ~3.5 | sorted_vector(linear) 48 ns | sorted_vector_soa 51 ns |
| 合成資料（深、30 萬掛單） | ~80 | dense_array + compact 111–122 ns | dense_array + OA 130–145 ns |
| Nasdaq 熱門 20 檔（很深、54 萬掛單） | ~1,900 | dense_array + OA + prefetch 117 ns | dense_array + ankerl 125 ns |

B-tree 兩邊都不是第一但都不差，是最「穩」的通用選擇；若要兩種情況都最快，下一步可以做混合結構
（best 附近用小陣列/稠密視窗，遠端價位放 B-tree 或排序陣列）。
注意：NYSE 樣本的 dense_array 是在 tick 修正前測的，需用一分錢 tick 重測。

## 結果 6：最終比較與推薦方案

這一輪做的改動：

- **dense_array 改用一分錢 tick**（`--tick cent`，預設）：NYSE 樣本從 90 ns / 587 MB 變成 48 ns / 36 MB。
  先前「dense_array 有大量 overflow 配置（0.14 次/事件）」的說法是測試設定錯誤造成的（那次只載入中段的小時檔，
  缺少 symbol mapping，tick 退回 1）；正確設定下 overflow 只有 0.003–0.005 次/事件。
- **混合結構**：`ArrayLevels` 的視窗上限、遠端容器、名稱改為模板參數，並加入**重新置中**（較好的價格落在已滿的視窗外時，
  把視窗移過去）與依價格範圍搬移；新增 `hybrid(4K window+btree)`、`hybrid(1K window+btree)`。
- **Replace 原地處理**：沿用同一個 order 節點，只換 index key，重新排到該價位隊尾；價格不變時不刪除/重建價位。
- **自適應 prefetch**：`--prefetch-min-orders N`，掛單數少於 N 時不 prefetch（小 book 在 cache 裡，prefetch 只有成本）。

三份資料、18 個變體、同一版程式（`results/final_*.txt`；NYSE 與合成資料 `--repeat 3`，ITCH `--repeat 2`；
prefetch k=4、門檻 5 萬筆）。每格為「一般 / 自適應 prefetch」ns/event；最後一欄是三份資料相對各自最佳值的幾何平均（1.000 = 每份都最快）。

| variant | NYSE 2019（淺） | 合成（中） | Nasdaq 熱門 20 檔（深） | 綜合 |
|---|---|---|---|---|
| **dense_array + open_addressing** | 48.0 / 50.3 | 143.7 / **97.5** | 117.5 / **98.8** | **1.028** |
| hybrid(4K window+btree) + open_addressing | 46.7 / 51.3 | 145.8 / 100.6 | 121.7 / 103.2 | 1.045 |
| hybrid(1K window+btree) + open_addressing | 48.4 / 53.2 | 131.6 / 97.2 | 141.8 / 114.6 | 1.082 |
| dense_array + compact_fp(8B) | 52.1 / 49.9 | 122.5 / 102.8 | 109.8 / 117.2 | 1.098 |
| dense_array + absl::flat_hash_map | 52.6 / 53.0 | 157.4 / 110.5 | 112.0 / 109.4 | 1.144 |
| dense_array + ankerl::unordered_dense | **44.6** / 48.3 | 189.0 / 154.9 | 106.3 / 106.8 | 1.200 |
| absl::btree_map + open_addressing | 61.5 / 60.3 | 158.4 / 105.5 | 188.8 / 147.7 | 1.302 |
| sorted_vector(linear) + open_addressing | 48.2 / 51.1 | 156.4 / 96.5 | 286.2 / 242.5 | 1.384 |
| std::map + open_addressing | 63.6 / 66.9 | 161.3 / 122.0 | 203.4 / 177.2 | 1.479 |
| std::map + unordered_map | 89.1 / 90.3 | 370.3 / 343.2 | 417.6 / 424.0 | 3.108 |

### 推薦方案

**`dense_array`（一分錢 tick、std::map overflow）+ `open_addressing` index + intrusive FIFO list + 自適應 prefetch（k=4，掛單 ≥ 5 萬筆才啟用）**

- 三份資料綜合最佳（1.028）：淺 book 與最佳差 8% 以內（44.6 vs 48.0），中、深 book 都是第一或並列第一（97.5、98.8 ns）。
- 尾端延遲也在前段：p99 160 / 580 / 497 ns（三份資料）。
- 記憶體：NYSE 36 MB、Nasdaq 熱門 20 檔約 150 MB；需要更省時改用 `hybrid(4K window+btree)`（綜合 1.045，只慢約 2%）。
- 實作要點：tick 用實際價格格點（不要用 MPV）；index 用 open addressing 或 compact（compact 不開 prefetch 時更好）；
  排隊用 intrusive list（vector_queue 在三份資料都較慢）；`std::unordered_map` 一律避免（慢 2–3 倍）。

沒有入選的原因：

- `sorted_vector(linear)`：淺 book 很好，但深 book（每邊上千價位）慢 2.5 倍，p99 到 1.7 µs。
- B-tree / std::map：各種資料都穩定但都不是前段（綜合 1.30 / 1.48）。
- `ankerl::unordered_dense`：淺、深都很好，但在合成資料（大量隨機刪單）明顯變慢，且沒有 prefetch 介面。
- `vector_queue`：三份資料都比 intrusive list 慢。
- 換 allocator：熱路徑幾乎不配置記憶體，影響在雜訊內。

Replace 原地處理的效果：樹與排序陣列類的 replace 快 10–17%（std::map 369 → 305 ns）；dense_array 本來建立/刪除價位就是 O(1)，
在雜訊內沒有差別。

## 結果 7：爆量封包內的延遲（Nasdaq ITCH 熱門 20 檔，模擬封包）

問題：行情劇烈波動時一個封包帶 30–40 筆訊息，封包後段的訊息要等前面全部處理完，p99 被拉高。

封包重建（`obl_itch extract` 同時輸出 `.pkt`）：ITCH 樣本檔沒有 MoldUDP64 封包資訊，所以用整天的完整訊息流近似——
同一撮合時間戳的連續訊息放同一封包，上限為 MoldUDP64 payload（1,452 bytes），再只保留這 20 檔的訊息。
結果：9,376 萬個封包，99.5% 只有 1 筆；**≥20 筆的封包 11,559 個，最多 46 筆**。

`obl_burst` 逐封包重播，每筆訊息的延遲 = 從開始處理該封包到這筆訊息的結果被「發佈」（讀該檔最佳買賣價寫入輸出 ring，
可再加 `--publish-ns` 模擬下游工作）。比較四種做法（dense_array + open addressing）：

- **per-message**：處理一筆、發佈一筆
- **prefetch**：整包往前看，滾動兩階段 prefetch（index 提前 2K 筆、order 節點提前 K 筆，K=8）
- **conflate**：整包處理完，每檔受影響的股票只發佈一次
- **prefetch + conflate**：兩者一起

≥20 筆封包的**最後一筆**與**第一筆**延遲 p50（µs，三次執行的範圍；原始輸出 `results/2025-12-08_itch_top20_burst_runs2-3.txt`）：

| 做法 | 最後一筆（發佈只有寫 BBO） | 第一筆 | 最後一筆（每次發佈 +100 ns 下游） | 第一筆 |
|---|---|---|---|---|
| per-message | 6.3–7.3 | 0.13–0.17 | 10.5–11.3 | 0.30–0.32 |
| prefetch | 3.7–4.6（−35~45%） | 0.29–0.34 | 8.1–8.5（−25%） | 0.46–0.49 |
| conflate | 4.1–5.0（−30%） | = 最後一筆 | 4.4–5.3（−55%） | = 最後一筆 |
| **prefetch + conflate** | **2.7–3.6（−50~57%）** | = 最後一筆 | **3.0–4.0（−65~70%）** | = 最後一筆 |

- 所有訊息整體的 p50 幾乎不變（99.5% 的封包只有 1 筆），改善集中在爆量封包——正是 p99 的來源。
- **prefetch** 在爆量時效果比平時大：大單掃過多個深價位時，要碰的 order 和 index 多半不在 cache，整包往前預取正好讓這些 miss 重疊。
- **conflate** 的效益和下游成本成正比：下游每次 100 ns 時，40 筆省下約 39 次發佈。代價是**封包第一筆要等整包處理完**
  （0.15 µs → 3–5 µs），只看最後一筆的話是大幅改善，看第一筆則變差。
- **限制**：這些爆量封包只有約 1.15 萬個，p99 由最差的約 115 個封包決定，在這台 VM 上每次執行差到數倍（12–170 µs），
  **p99 無法在這裡可靠量測**，以上只用 p50 下結論；p99 需要在實體機（隔離核心、固定頻率）上量。

## 下一步

- [x] 取得真實 NYSE XDP 樣本驗證 layout 與分佈（結果 4）
- [x] dense_array 改用一分錢 tick（`--tick cent`，預設）
- [x] 熱門商品真實資料：Nasdaq ITCH 熱門 20 檔（結果 5）
- [x] NYSE 樣本用一分錢 tick 重測 dense_array（結果 6）
- [x] 混合價位結構：稠密視窗 + B-tree，含重新置中（結果 6）
- [x] Replace 原地處理、自適應 prefetch（結果 6）
- [ ] 在實體機（固定頻率、隔離核心、perf）重測，確認 5% 等級的差異與爆量封包的 p99
- [ ] 用真實 OMD-C 錄檔（含真實封包切割與硬體時間戳）重跑 `obl_burst`
- [ ] 依真實分佈校準產生器（價位數、best 掛單數、新單落點、事件比例）
- [x] B-tree / SoA / pool-alloc levels、ankerl / absl index、prefetch、allocator × THP
- [x] 減少 cache miss：order 節點 32 bytes、compact fingerprint index、prefetch 用的 `peek()`
- [x] 每種事件類型分開量測（rdtsc，`--latency`）
- [x] 每價位陣列佇列（`VectorQueues`）：比雙向鏈結慢，見結果 3
- [ ] 讓合成資料的價位深度更接近真實（目前最熱價位 ~3,400 筆掛單，偏多）；或用真實樣本校準分佈
- [ ] 在較安靜的機器（isolcpus / 固定頻率）重跑，降低雜訊
- [ ] 量測拆解：perf counters（cache miss / branch miss）per event type
- [ ] OMD-C SF adapter（30/31/32/33/34/50…）
