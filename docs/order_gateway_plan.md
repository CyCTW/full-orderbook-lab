# Order Gateway 研究規劃

目標：研究低延遲交易系統中負責「把單送進交易所」的 order gateway，
先盤點它該有的功能，再挑出值得在這個 repo 實測的部分（延續 L3 book 實驗的做法：
多個實作變體 + 同一份資料重播 + 量延遲分佈）。

本文件只是規劃，尚未有程式碼。

## 1. 在系統中的位置

```
market data (XDP / ITCH / OMD-C)
  └─ feed handler + L3 book（本 repo 已完成）
       └─ strategy        決定 送 / 改 / 撤
            └─ order gateway   ← 本次研究對象
                 ├─ pre-trade risk
                 ├─ order state / ID 對應
                 ├─ 協定編碼（OUCH / OCG-C / FIX / iLink3 SBE…）
                 ├─ session 管理（login、heartbeat、序號、重送）
                 └─ 傳輸（kernel TCP / kernel bypass / FPGA）
                      └─ 交易所 gateway → matching engine
                           └─ ack / fill / reject 回程 → gateway → strategy、風控、position
```

關鍵延遲指標：**tick-to-trade**（行情封包進網卡 → 委託封包出網卡）。
gateway 佔的是「strategy 決策完成 → 封包離開」這一段，以及回報的「封包進來 → strategy 看到 fill」這一段。

部署型態有兩種，會影響設計：

| 型態 | 說明 | 取捨 |
|---|---|---|
| in-process（library） | strategy 與 gateway 同執行緒，函式呼叫直接送單 | 最快，但每個 strategy 各自持有 session、風控難集中 |
| 獨立行程 | strategy 經 shared-memory SPSC/MPSC ring 交給 gateway 行程 | 多策略共用 session 與集中風控；多一次跨核 cache line 傳遞（約數十～百 ns） |

## 2. 功能盤點

### 2.1 Session / 連線管理

- 登入 / 登出、帳號認證、heartbeat / test request、斷線偵測（timeout）
- **序號管理與斷線恢復**：sequence number 持久化；重連後的重點是**向交易所補收**斷線期間漏掉的回報
  （ack / fill / 交易所主動撤單），把 order state 對齊，而不是補送舊委託
  - OUCH（SoupBinTCP）：只有交易所→客戶端有序號，login 帶要求的序號即可重播；客戶端送出的委託沒有序號、不重送
  - FIX：雙向都有序號；對方要求重送時，過時的委託應以 SequenceReset-GapFill 跳過，而不是真的重送
  - 斷線前已送出、未收到 ack 的委託屬未知狀態，要等重播回報（或查詢委託狀態）才能確定
  - 斷線期間 strategy 送來的新單：**直接拒絕回 strategy**（原因：session down），不排隊；
    恢復並對帳完成後通知 strategy，由它依當下行情重新決策
- 多 session：交易所通常限制每 session 的訊息速率，需要把單分散到多條 session
- Primary / backup gateway 切換（交易所端多個 gateway IP；本地端熱備援）
- Cancel-on-disconnect 設定（斷線時交易所自動撤單）
- 交易時段狀態（開盤前、競價、連續交易、收盤競價），不同時段可送的單型不同

### 2.2 協定編碼 / 解碼

- 委託類：New Order、Cancel、Replace（改價改量）、Mass Cancel
- 回報類：Accepted / Rejected / Replaced / Canceled / Executed / Broken trade / Business reject
- 單型與屬性：limit、market、IOC / FOK、post-only、iceberg、short-sell 旗標、capacity、帳號欄位…
- 編碼方式差異：FIX tag=value（文字、需轉數字字串與 checksum）vs 固定 layout binary（OUCH、OCG-C、SBE）
- 低延遲手法：**預先編好的訊息模板**，送單時只 patch 價格 / 數量 / ID 欄位

### 2.3 Order state 與 ID 管理

- ClOrdID 產生（每日唯一、單調遞增；交易所常限制長度與字元）
- ClOrdID ↔ 交易所 OrderID ↔ strategy 內部 order 的對應
- 狀態機：PendingNew → New → PartiallyFilled → Filled；PendingCancel / PendingReplace → Canceled / Replaced；Rejected
- **in-flight 狀態**：送出未 ack 時的數量 / 價格（風控要算「最壞情況」曝險）
- 例外處理：cancel 與 fill 交錯（cancel reject: too late）、replace 途中成交、主動撤單（交易所端）、重複 / 亂序回報
- 成交回報即時更新 position 與 PnL，餵回 strategy 與風控

### 2.4 Pre-trade risk（風控）

法規面：美國 SEC Rule 15c3-5（Market Access Rule），香港 SFC 對電子交易亦有類似要求；
實務上券商與自營商都必須在送單路徑上做檢查，**這些檢查本身就在 hot path 上**。

| 檢查 | 說明 |
|---|---|
| 單筆數量 / 金額上限 | fat finger |
| 價格偏離（price collar） | 與參考價（BBO、最近成交、前收）偏離過大即拒絕——可直接用本 repo 的 book |
| 持倉 / 曝險上限 | 含 in-flight 委託的最壞情況 |
| 信用 / 資金額度 | 每帳號累計 |
| 委託速率上限 | 每秒送單 / 撤單數，防策略失控 |
| 重複單偵測 | 短時間內相同參數的單 |
| Self-trade prevention | 自己的買賣單互撮 |
| 限制名單、賣空規則 | restricted list、Reg SHO locate / HK 可賣空名單、tick size 合法性 |
| Kill switch | 一鍵全撤並封鎖送單（手動、或觸發條件自動） |

### 2.5 Throttle / 流量控制

- 交易所限制每 session 每秒訊息數，超過會被拒或斷線
- token bucket / sliding window 實作；超量時排隊還是直接拒絕
- 排隊時的優先序：**撤單優先於新單**（降低風險）、同一張單的多次改價可合併（只送最新的）

### 2.6 傳輸層與低延遲技術

- kernel TCP：`TCP_NODELAY`、busy polling（`SO_BUSY_POLL`）、避免 Nagle 與 delayed ACK 交互作用
- kernel bypass：Solarflare/AMD Onload / ef_vi / TCPDirect、DPDK + 使用者空間 TCP、Mellanox VMA
- 預先組好的 TCP/IP 封包、只 patch payload 與 checksum；網卡預熱（warm-up / 送假封包保持 cache 與 TX path 熱）
- FPGA / SmartNIC：送單與風控整個下放到硬體（業界 tick-to-trade 個位數到數十 ns 等級）
- 一般 hot path 原則：無動態配置、無 syscall（bypass 時）、無鎖、CPU pinning / isolcpus、hot data 放在少數 cache line

### 2.7 觀測、稽核與營運

- 時間戳：軟體 `rdtsc` / 硬體網卡 timestamp，量 decide→send、send→ack、tick-to-trade
- 稽核 log：每筆送出 / 收到的訊息都要留存（法規），但必須**非同步**——hot path 寫入 SPSC ring，另一核負責落地
- Drop copy 與日終對帳（與交易所 / 券商成交紀錄核對）
- 監控：session 狀態、拒單率、延遲分位數、throttle 使用率
- 設定熱更新（風控參數、kill switch）不能停機

### 2.8 測試

- **交易所模擬器**：接受委託、回 ack / fill；撮合可以直接拿本 repo 的 L3 book 改成 matching engine
- 交易所提供的 conformance / certification 測試情境
- 協定解析 fuzzing；斷線、序號跳號、亂序回報的故障注入
- 以真實行情（ITCH / XDP 樣本）驅動的端到端重播

## 3. 本 repo 可實測的研究題目

容器內沒有 kernel bypass 網卡與 FPGA，所以聚焦在「軟體 hot path」的部分，
每題都做成可替換的變體並量 p50 / p99 / p99.9：

| # | 題目 | 變體 | 預期觀察 |
|---|---|---|---|
| G1 | 訊息編碼成本 | FIX 文字逐欄組字串 / FIX 模板 patch / binary struct 填值 / binary 模板 patch | 文字數字轉換與 checksum 的成本；模板的效益 |
| G2 | Order state 表 | `unordered_map<ClOrdID>` / open addressing / **ClOrdID 低位元直接當陣列 index**（ID 由我們產生，可設計成 O(1)） | 與 book 的 order index 實驗呼應 |
| G3 | 風控檢查成本 | 逐項 if / 合併成 branchless 計算 / 依失敗機率排序 | 全套檢查能否壓在 ~50 ns 內 |
| G4 | Throttle 實作 | token bucket（rdtsc）/ sliding window ring / 撤單優先佇列 | 正常流量下近乎零成本；爆量時行為 |
| G5 | strategy → gateway 傳遞 | 同執行緒呼叫 / SPSC ring 跨核 / 跨 NUMA | 跨核 cache line 轉移的實際代價 |
| G6 | 稽核 log | 同步 `write` / SPSC ring + logger 執行緒 / 只記 binary 原始訊息 | log 對 hot path 尾延遲的影響 |
| G7 | 送出路徑（kernel TCP，loopback） | `send` / `writev` 批次 / busy poll 收回報 | syscall 成本與尾延遲；作為 kernel bypass 的對照基準 |
| G8 | 端到端 tick-to-trade | ITCH 重播 → book → 玩具策略 → gateway → 模擬交易所 | 各段延遲拆解，找出最大的一段 |

## 4. 分階段計畫

1. **選協定與規格**：初版採 **HKEX OCG-C Binary Trading Protocol v3.2**（規格在 HKEX 網站公開），
   格式、checksum 與第一批量測見 [`ocgc_binary.md`](ocgc_binary.md)。之後可再加 FIX 作為文字協定對照。
2. **骨架**：`include/obl/gw/`——協定 encoder/decoder（OCG-C 訊息與 Execution Report 已完成）、session 狀態機（已完成，見 `ocgc_binary.md` §6）、order state 表；單元測試先行。
3. **模擬交易所**：以 L3 book 為核心的 matching engine，走 loopback TCP，回傳 OUCH 回報。
4. **G1–G4**：純 CPU 的微基準（不經網路），沿用 `bench/` 的量測框架與 fork 隔離。
5. **G5–G7**：跨執行緒與 socket 的量測。
6. **G8**：端到端整合與延遲拆解，結果寫進 README（延續「結果 N」格式）。

## 5. 待決定

- 部署型態：先做 in-process library，還是一開始就做獨立行程 + shared-memory ring？
- 範圍：是否包含 FIX（文字協定對照組）與 drop copy / 對帳？
