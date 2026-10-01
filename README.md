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

## 下一步

- [ ] 取得真實 NYSE XDP 樣本驗證 layout 與分佈
- [x] B-tree / SoA / pool-alloc levels、ankerl / absl index、prefetch、allocator × THP
- [x] 減少 cache miss：order 節點 32 bytes、compact fingerprint index、prefetch 用的 `peek()`
- [x] 每種事件類型分開量測（rdtsc，`--latency`）
- [x] 每價位陣列佇列（`VectorQueues`）：比雙向鏈結慢，見結果 3
- [ ] 讓合成資料的價位深度更接近真實（目前最熱價位 ~3,400 筆掛單，偏多）；或用真實樣本校準分佈
- [ ] 在較安靜的機器（isolcpus / 固定頻率）重跑，降低雜訊
- [ ] 量測拆解：perf counters（cache miss / branch miss）per event type
- [ ] OMD-C SF adapter（30/31/32/33/34/50…）
