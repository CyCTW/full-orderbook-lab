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

| Levels | 說明 |
|---|---|
| `std::map` | 紅黑樹，baseline |
| `sorted_vector(linear)` | 連續陣列，best 在尾端，從尾端線性搜尋 |
| `sorted_vector(binary)` | 同上，二分搜尋 |
| `dense_array` | 以 tick 為索引的稠密陣列，O(1) 找價位；bitmap 找下一個 best；超出視窗/非整 tick 價格進 overflow map |

| Index | 說明 |
|---|---|
| `unordered_map` | std baseline（node-based） |
| `open_addressing` | linear probing、2 的冪容量、load ≤ 0.5、backward-shift 刪除（無 tombstone） |

新增變體：寫一個符合 `MapLevels` 介面的 Levels（`get_or_create / update / best / for_each / size / clear / set_tick`）
或符合 `StdOrderIndex` 介面的 Index，加到 `variants.hpp` 的 `for_each_variant`，測試與 benchmark 會自動涵蓋。

## 建置與使用

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build                     # 含與 naive reference book 的差分測試

# 合成資料（XDP 格式 PCAP）
./build/obl_gen --out data/synth_5m.pcap --messages 5e6 --symbols 300

# 檢查 capture（訊息統計、序號、book 一致性）
./build/obl_dump data/synth_5m.pcap --print 20

# Benchmark：每個變體在 fork 出的子行程跑（乾淨 heap、獨立量 RSS），結束時比對全深度 + 佇列順序 checksum
./build/obl_bench data/synth_5m.pcap --repeat 3 --latency [--reserve 1000000] [--only dense]
```

真實 NYSE 樣本：下載 `ftp.nyse.com/Real Time Data Samples/NYSE XDP/NYSE_IBF/` 的檔案放到 `data/`，
`obl_dump`/`obl_bench` 可直接讀 `.pcap.gz`。A/B 線預設以 UDP port 做仲裁（`--key-by-group` 改為 group+port）；
`--port P` 只看單一 channel。pcapng 需先 `editcap -F pcap` 轉檔。

## 合成資料模型（`include/obl/sim/generator.hpp`）

- 事件比例預設 add 45% / delete 38% / modify 7% / replace 5% / execution 5%
- 新單離對手 best 的距離 ~ geometric(0.3) tick，1% 落在 200–3200 tick 外（製造深而稀疏的尾巴）
- Symbol 活躍度 Zipf 分佈；每個 symbol 有穩態掛單量目標；價格對錨點均值回歸，不會交叉
- OrderID 不連續（模擬 OMD-C）；modify 有保留/失去排隊位置兩種（PositionChange）
- 生成器自帶 shadow book，測試會驗證所有訊息都引用存在的委託

## 初步結果

合成資料 5M 訊息、300 symbols、結束時約 30 萬筆掛單；4 vCPU Xeon @ 2.1GHz VM（有雜訊，重跑差 10–20%）：

| variant | ns/event (best of 3) | e2e ns/msg | p50 / p99 / p99.9 (ns) |
|---|---|---|---|
| std::map + unordered_map | 378 | 504 | 617 / 1799 / 18029 |
| std::map + open_addressing | 181 | 266 | 257 / 839 / 5561 |
| sorted_vector(linear) + open_addressing | 169 | 246 | 227 / 670 / 5242 |
| sorted_vector(binary) + open_addressing | 163 | 233 | 235 / 660 / 4596 |
| dense_array + unordered_map | 325 | 439 | 413 / 1198 / 14746 |
| dense_array + open_addressing | 149 | 184 | 166 / 582 / 6004 |

觀察：order index 是最大的單一因素（std::unordered_map → open addressing 約快 2 倍）；
價位容器之間差距較小，因為隨機刪單造成的 order/index cache miss 主導成本。

## 下一步

- [ ] 取得真實 NYSE XDP 樣本驗證 layout 與分佈
- [ ] 更多變體：B-tree levels、order 節點內嵌 level 指標、per-symbol index、huge pages / prefetch
- [ ] 量測拆解：perf counters（cache miss / branch miss）per event type
- [ ] OMD-C SF adapter（30/31/32/33/34/50…）
