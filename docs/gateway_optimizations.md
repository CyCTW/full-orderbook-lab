# Order gateway 的延遲優化

本文件記錄 order gateway 送單與收回報路徑上用到的每一項優化：在程式哪裡、用了什麼手法、為什麼，以及單獨量測的效果。
§1–§2 是說明，§3 起是量測。

## 1. 送單路徑與目前的延遲

從 strategy 呼叫 `new_order()` 到訊息 bytes 交給傳輸層（`bench/gw_bench.cpp`，所有風控開啟、簿上 20 張自己的掛單；
原始輸出 `results/gw/2026-10-03_send_path_run{1,2,3}.txt`）：

| 路徑 | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| `new_order()` → bytes 交給傳輸層（純軟體） | **83 ns** | 207–247 ns | 420–470 ns |
| `new_order()` 整個呼叫（含送出後的記帳） | 106 ns | 286–298 ns | 563–604 ns |
| `new_order()` → kernel `send()` 返回（loopback TCP） | 3.6–6.2 µs | 12–18 µs | 47–59 µs |

```
new_order()                         gateway.hpp       1  session 是否 up
 ├─ RiskEngine::check_new()         risk.hpp          2  風控           58 ns（自成交檢查佔 38）
 ├─ OrderTable::new_order()         order_state.hpp   3  ID、slot、曝險  18 ns
 ├─ Throttle::admit()               throttle.hpp      4  流量控制       2.5–10 ns
 └─ send_new() → Session::send_new_order()
     ├─ NewOrderTemplate::fill_regs()  order_template.hpp  5  編碼 + CRC   21–25 ns
     ├─ xmit() → Transport::send()     session.hpp         ← bytes 交出去
     └─ MessageStore::append()、AuditLog::log()、計數器      送出後才做
```

各段單獨量的時間加總會比整條路徑多（每段都多付一次計時成本，也無法和前後段重疊執行）。
目前最大的成本是 kernel `send()`，只有 kernel bypass 能大幅降低。

## 2. 優化目錄

### 2.1 送單路徑

| 優化 | 位置 | 手法 | 為什麼 |
|---|---|---|---|
| 預先編好的訊息模板 | `order_template.hpp` `NewOrderTemplate` | 每個（股票、方向、TIF）一個 184-byte 模板，140 bytes 固定不動；送單只改序號、編號、時間、價量 5 段，原地覆寫 | 不必每筆都組 presence map 與所有字串欄位 |
| 硬體 CRC32C | `crc32c.hpp` `crc32c_hw` | SSE4.2 `crc32` 指令，每條處理 8 bytes | OCG-C 規定的 CRC32C 剛好是該指令的多項式；查表版慢 16 倍 |
| 增量 CRC | `crc32c.hpp` `Crc32cShift`、`order_template.hpp` `fill_regs` | 模板的變動段先填 0 算出基準 CRC；送單時只算 4 段的 CRC，用 `pclmulqdq` + `crc32` 補上後面的零 byte 再 XOR 合併；常數 x^(8k−33) mod P 建模板時算好 | 一條 23 步的相依長鏈變成 4 條可並行的短鏈 |
| 欄位在暫存器組好 | `order_template.hpp` `clordid_word`、`time_words` | 數字用兩位數查表組成 64-bit word，寫一次，CRC 直接吃暫存器 | 避免「多筆小寫入後用大讀取讀回」的 store-forwarding 失敗 |
| 委託編號固定 8 位數 | `order_state.hpp` `first_req_id = 10,000,000` | 從 10,000,000 起連續配發 | 規格禁止前導零；固定長度才能整段覆寫、讓增量 CRC 的區段固定 |
| 委託編號當陣列索引 | `order_state.hpp` `alloc_req`、`lookup` | `id − 起始值` 就是請求表索引；邊界檢查用無號減法一次比較 | 不用 hash：送單寫入、收回報查詢都是一次陣列存取 |
| 冷熱資料分離 | `order_state.hpp` `Order`（64 B）、`Cold` | 每筆單的熱欄位剛好一條 cache line；交易所 ID、標籤、串列指標放另一陣列 | 同樣的 cache 放得下兩倍的委託 |
| slot 依序配發 | `order_state.hpp` `new_order` | 遞增配發、當天不回收 | 不需要 free list；下一筆緊接上一筆，硬體預取猜得中 |
| 曝險、帳戶總額維護差額 | `order_state.hpp` `add_exposure`、`gross_open_notional_` | 每次狀態變化「扣舊貢獻、改狀態、加新貢獻」 | 風控讀一個數字，成本與活單數、商品數無關 |
| 價格偏離用整數交叉相乘 | `risk.hpp` `check_collar` | `diff × 10000 > bps × 參考價`（128-bit） | 沒有除法、沒有浮點 |
| 定點整數價量 | 全部 | 8 位小數的 int64，與 OCG-C 線上格式相同 | 送出時不必轉換 |
| Kill switch 用 relaxed atomic | `risk.hpp` `KillSwitch` | 送單路徑上一次普通讀取 | 任何執行緒都能觸發，送單路徑幾乎不付成本 |
| GCRA 速率限制 | `rate_limit.hpp` `Gcra` | token bucket 濃縮成一個時間戳 | 一次比較 + 一次加法，不需要定時補 token |
| 滑動視窗 | `rate_limit.hpp` `SlidingWindow` | 固定大小環狀陣列存最近 N 筆的時間 | O(1)，不配置記憶體 |
| 先送出、後記帳 | `session.hpp` `send_new_order` | 存檔、稽核、計數器都放在 `send()` 之後 | 這些操作的 cache miss 不再延後封包 |
| 存檔區預先寫過一遍 | `session.hpp` `MessageStore::reserve(prefault)` | 啟動時把整塊記憶體寫一遍 | 送單路徑不會遇到 page fault |
| 所有表格啟動時配好 | `OrderTable` 建構子、`Throttle`、`SpscRing` | `resize()` 並初始化 | 送單路徑不配置記憶體、不遇到 page fault |
| 單寫入者計數器 | `metrics.hpp` `Counter` | relaxed load + store | x86 上是普通加法，不是 `lock add` |
| 稽核 log 交給別的執行緒 | `audit.hpp`、`spsc.hpp` `SpscRing` | 送單執行緒只把訊息複製進 SPSC ring | 寫檔的 syscall 與 I/O 不在送單執行緒上 |
| SPSC 索引分開 cache line、快取對方位置 | `spsc.hpp` | 生產端與消費端各一條 cache line，各自記住對方上次的位置 | 正常情況下兩個核心不搶同一條 cache line |
| 單一執行緒擁有狀態 | 架構 | gateway、session、委託表、風控只在一條執行緒上改 | 送單路徑不加鎖 |
| template 靜態分派 | `OcgcGateway<Transport, Listener>`、`Session<…>` | 沒有 virtual function | 編譯器能把整條路徑 inline |
| `if constexpr` + `requires` 的 hook | `session.hpp` `xmit` | 稽核 hook 是否存在在編譯時決定 | 沒掛 hook 時連分支都不產生 |
| `[[likely]]` / `[[unlikely]]` | `gateway.hpp`、`risk.hpp`、`session.hpp` | 標記常態路徑 | 錯誤處理移出去，常態路徑的指令連續 |
| TCP_NODELAY、可選 SO_BUSY_POLL | `net/tcp.hpp` `tune` | 關 Nagle；收資料時 kernel 輪詢網卡 | 小封包不被延遲合併 |
| `-O3 -march=native` | `CMakeLists.txt` | | 啟用 `crc32`、`pclmulqdq` 等指令 |

### 2.2 收回報路徑

| 優化 | 位置 | 手法 | 為什麼 |
|---|---|---|---|
| 欄位存在表一次讀 64 bits | `protocol.hpp` `for_each_field_unchecked` | 32 bytes 當 4 個 big-endian word，用 `countl_zero` 跳到下一個存在的欄位 | 一則回報只有 ~20 個欄位，不必逐一檢查 256 個 bit |
| 字串欄位用到才算長度 | `messages.hpp` `LazyStr` | 解碼只記指標與容量，讀取時才找 NUL | 策略通常只看編號與數量 |
| 先驗 frame 再解碼、只驗一次 CRC | `session.hpp` `consume`、`protocol.hpp` `valid_frame` | session 驗一次，之後的解碼用 `_unchecked` 版本 | 不重複算 CRC |
| 收到完整訊息時不複製 | `session.hpp` `on_bytes` | 直接從呼叫端的 buffer 解析，只有不完整的尾巴才存進固定大小的接收緩衝區 | 常態不多一次 memcpy，也不配置記憶體 |
| 回報用編號一次查表找到委託 | `order_state.hpp` `apply` | 撤單 / 改單的編號直接指向原委託的 slot | 不必先解析 Original Client Order ID |
| 成交用累計量去重 | `order_state.hpp` `apply` | CumQty 沒增加就是舊回報 | 不需要 Execution ID 的 hash set |

## 3. 對照量測：總表

每項優化單獨和它取代的直覺寫法比較，同一台機器、同一種量法：每次呼叫前後用 fenced rdtsc，扣掉計時本身的成本，
取 p50 / p99。環境：雲端 VM，Intel Xeon 2.8 GHz（L1d 32 KiB、L2 1 MiB / 核，L3 33 MiB），綁單核，每列 30 萬次呼叫，
跑 3 次，表中為 3 次的範圍。

程式：`bench/gw_ablation.cpp`（A–H）、`bench/ocgc_bench.cpp`（CRC、編碼、解碼）。
原始輸出：`results/gw/2026-10-03_ablation_run{1,2,3}.txt`、`results/ocgc/2026-10-03_rerun_after_gateway.txt`。

| 優化 | naive | 優化後 | 倍數 | 主要效益 |
|---|---|---|---:|---|
| **整則 New Order 編碼 + checksum** | Writer 逐欄組 + `snprintf` + 查表 CRC：p50 665–796 ns，p99 1.0–1.2 µs | 模板 + `fill_regs`：p50 20 ns，p99 26–31 ns | **~35×** | 下面 4 項的合計 |
| └ CRC 演算法（180 B） | 逐 byte 查表 438 ns（逐 bit 1,815 ns） | 硬體 `crc32` 27 ns → 增量 CRC 13 ns | 16× / 33× | 最大的一項 |
| └ 數字轉字串 | `snprintf`：整則 253–497 ns | 兩位數查表：整則 50–73 ns | ~5× | 第二大 |
| └ 預先編好的模板 | 從頭組（查表 + 硬體 CRC）50–73 ns | 模板 + 硬體 CRC 36–38 ns | ~1.6× | 140 / 184 bytes 不必每次寫 |
| └ 欄位在暫存器組好（避免 store-forwarding 失敗） | 寫入後重讀：p50 31–34 ns，p99.9 ~170 ns | p50 25–26 ns，p99.9 40–60 ns | 1.3×，尾端 3–4× | 主要是壓尾延遲 |
| **委託編號 → 委託（送單時寫入）** | `unordered_map`：p50 30 ns，p99.9 1.8–3.7 µs（rehash） | 陣列索引 2.5–3.2 ns，p99.9 ≤ 98 ns | **~10×**，尾端 20× 以上 | 沒有 hash、沒有 rehash |
| **委託編號 → 委託（收回報時查詢）** | `unordered_map`：p50 16–19 ns，p99 338–351 ns | 3.2–3.9 ns，p99 9–29 ns | **~5×**，p99 ~15× | hash 桶串列的 cache miss |
| **委託編號固定 8 位數**（只看轉字串本身） | `to_chars` 變長 10.4 ns（`snprintf` 42 ns） | 7.9 ns | 1.3× | **速度不是重點**，見下方說明 |
| **冷熱資料分離**（64 B vs 128 B 的委託紀錄，隨機更新一筆） | 16k 筆：25–28 ns | 16k 筆：11.8 ns | **~2.2×**（只在 L2 邊界） | 其他規模差異小或不穩定，見下方 |
| **在途曝險：維護總和 vs 每次加總活單** | 10 / 100 / 1000 張活單：6.4 / 62 / 800–1070 ns | 0.7–1.8 ns | 4× / 40× / 500× 以上 | 與活單數無關 |
| **帳戶總額：維護總和 vs 每次掃所有商品** | 10 / 100 / 1000 檔：7–11 / 48–89 / 610 ns | 1.8 ns | 5× / 30× / 340× | 與商品數無關 |
| **計數器（10 次 +1）** | `fetch_add`（lock xadd）57 ns | relaxed load + store 6.4 ns | **~9×** | 不需要鎖住匯流排 |
| **先送出、後記帳**（到 bytes 交出去的時間） | 記帳在前：p50 19.3 ns，p99 63–144 ns | 記帳在後：p50 10.0 ns，p99 12–24 ns | 2×，p99 3–6× | 存檔的 cache miss 不再擋在封包前面 |
| **存檔區預先寫過一遍** | 只 `reserve()`：p99 22–27 µs | p99 145–161 ns | **~150×（p99）** | page fault 從送單路徑消失 |
| **presence map 走訪**（收回報） | 逐一測 256 個 bit：246 ns | 64 bit 一組 + count-leading-zeros：49–53 ns | ~4.7× | |
| **字串欄位延後量長度**（收回報） | 解碼時每欄 `memchr`：68 ns | 用到才量：53 ns | 1.3× | |

## 4. 各項說明

### 編碼：35 倍裡各項的貢獻

從最直覺的寫法依序加上優化（p50）：

```
Writer + snprintf + 逐 byte 查表 CRC    665–796 ns
  → 換硬體 CRC                         253–497 ns   （CRC 從 ~440 降到 ~27）
  → snprintf 換兩位數查表                50–73 ns
  → 預先編好的模板                       36–38 ns
  → 增量 CRC（寫入後重讀）                31–34 ns
  → 欄位在暫存器組好                      20–26 ns
```

最大的兩步是 CRC 演算法（硬體指令）和數字轉字串。模板與暫存器那兩步省的絕對時間少，但尾延遲改善明顯
（p99.9 從 ~170 ns 降到 40–60 ns）。

### 委託編號固定 8 位數：效益不在速度

單看「把編號轉成字串」，固定 8 位數只比 `to_chars` 快 2.5 ns。它真正的作用是讓其他優化成立：

- **模板可以原地覆寫**：每次都完整蓋掉同一個 8-byte 區段。變長的話，短的編號會留下上一筆的殘餘數字，
  每次都得先清零或補 NUL 結尾。
- **增量 CRC 的區段固定**：要重算的位置與長度不變，事先算好的「補零」常數才能一直用。
- **規格**：不能有前導零，所以「補零到 8 位」不可行，只能從 10,000,000 起配發。

### 冷熱資料分離：只在特定規模有效

隨機更新一筆委託的 p50（ns）：

| 委託筆數 | 64 B 紀錄 | 128 B 紀錄 | 說明 |
|---:|---:|---:|---|
| 4,096 | 9.3 | 10.7–11.1 | 兩者都在 L2 內 |
| **16,384** | **11.8** | **25.4–27.9** | 64 B 剛好 1 MiB（在 L2 內），128 B 變 2 MiB（超出 L2）：**2.2 倍** |
| 65,536 | 39–147 | 80–144 | 3 次結果不一致 |
| 262,144 | 105–141 | 130–140 | 不一致 |
| 1,048,576 | 139–148 | 155–174 | 都在主記憶體，差 10–20% |

效益是「同樣的 cache 能放下兩倍的委託」，所以只在工作集剛好跨過某一層 cache 邊界時才明顯。
一個策略同時活著的單通常是幾十到幾千張，落在 L1 / L2 範圍內：在這個範圍差距不大，但也不會變差。
65k 和 262k 兩列在 3 次之間差異很大，推測是 VM 上 L3 被其他租戶共用，這兩列不下結論。

### 維護總和 vs 每次重算

「在途曝險」與「帳戶總額」改成在狀態變化時更新差額後，風控讀到的永遠是 1 個數字，成本固定 1–2 ns。
naive 版本的成本跟活單數或商品數成正比；在 1000 張活單或 1000 檔商品時，單是這一項就要 0.6–1 µs，
比整條優化後的送單路徑（83 ns）大一個數量級。

### 先送出、後記帳

記帳本身（存檔 + 稽核複製 + 計數器）平均不到 10 ns，但它寫的是之前沒碰過的記憶體，偶爾 cache miss，
放在送出前會讓 p99 從 12–24 ns 變成 63–144 ns。移到送出後，這些成本只延後下一筆單。

### 存檔區預先寫過一遍

只用 `reserve()` 保留記憶體時，第一次寫入每一頁都觸發 page fault，184 bytes 的訊息約每 22 筆碰到一次。
在這台 VM 上一次 page fault 要 20–40 µs（另一輪量測是 1.6–2.3 µs，VM 狀態不同差很多），p50 不受影響，p99 差兩個數量級以上。

## 5. 沒有單獨量的項目

- `[[likely]]` / `[[unlikely]]`、template 靜態分派（沒有 virtual function）、`-march=native`：都是編譯期的，
  沒做「關掉」的對照組。
- kill switch 的 relaxed atomic、GCRA、滑動視窗：本身都只有幾 ns，沒有和 naive 版本對照。
- **整個 gateway 的全 naive 版本沒有實作**，所以沒有「整條路徑 naive vs 優化」的實測數字。把上面各項 naive 版本的
  p50 相加（編碼 ~700、編號 map ~30、計數器 ~57、曝險與總額在 100 張單 / 100 檔時 ~110…），粗估 0.9 µs 上下，
  約是優化後 83 ns 的 10 倍；這只是估計，各項之間會互相影響（cache 競爭），實際值可能更高。


## 6. 尚未優化之處（依預期效益排序）

1. **Kernel bypass**：送單路徑 µs 等級的成本幾乎都在 `send()`，軟體部分只有約 0.08 µs。需要實體機與支援的網卡。
2. **自成交檢查是 O(該商品活單數)**：`risk.hpp` `crosses_own()` 走訪活單串列（pointer chasing），20 張掛單就要 38 ns，
   佔風控的三分之二。改成每個商品維護自己的最高買價、最低賣價即可 O(1)。
3. **撤單、改單沒有模板**：`messages.hpp` `encode_cancel` / `encode_amend` 用通用 Writer 從頭組，而且
   `Writer::finish()` 與 `Session::stamp()` 各算一次 CRC（共兩次）。可比照新單做模板 + 增量 CRC。
4. **委託表與存檔的 p99**（136–145 ns）：每筆都寫到沒碰過的 cache line。改成重複使用的環狀配置可讓資料留在 cache。
5. **`notional()` 的 128-bit 除法**：可能呼叫函式庫的 `__divti3`，可改成 64-bit 運算。
6. **`tmpl()` 每次確認 vector 大小並經過 `unique_ptr` 間接讀取**：可讓策略事先拿到模板參考。
7. **Warm-up**：以上數字都在「連續送單、cache 很熱」下量得；送單間隔很長時的第一筆延遲還沒量（規劃 G9）。
