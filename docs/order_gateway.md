# Order Gateway（HKEX OCG-C）：架構與功能

本文件說明 `include/obl/gw/` 下已完成的 order gateway：架構、各模組、對照 [規劃](order_gateway_plan.md) §2 的完成度、
主要設計決策，以及尚未處理的部分。協定細節與 checksum 量測見 [`ocgc_binary.md`](ocgc_binary.md)，
延遲優化與量測見 [`gateway_optimizations.md`](gateway_optimizations.md)。

送單路徑目前約 83 ns（p50，純軟體，到 bytes 交給傳輸層），詳見 [`gateway_optimizations.md`](gateway_optimizations.md)。

## 1. 架構

```
 strategy (同一執行緒)                                       control thread
   │ new_order / cancel / amend / mass_cancel                   │ ControlChannel.post(閉包)
   ▼                                                            ▼  (poll() 內執行)
 OcgcGateway  ── 一條 OCG-C session 一個 ─────────────────────────────────────────
   │ 1 session 是否 up            （斷線時直接回拒，不排隊）
   │ 2 RiskEngine.check_new       （共用：所有 session 同一份限額、kill switch）
   │ 3 OrderTable.new_order       （共用：request ID、order slot、曝險、部位）
   │ 4 Throttle.admit             （每 session 一個 1 秒滑動視窗；滿了就排隊）
   │ 5 NewOrderTemplate.fill_regs （預先編好的 184-byte 訊息 + 增量 CRC）
   ▼
 Session（OCG-C session 層，sans-IO）── 序號、登入、心跳、重送、gap fill
   │                     ▲
   ▼ Transport.send      │ on_bytes
 ConnTransport ── Connector（Lookup → primary → mirror → Lookup；TCP_NODELAY）── TCP
                         │
 回報：Execution Report → OrderTable.apply → RiskEngine（P&L、拒單率）→ Listener（strategy）
 每則進出訊息 → AuditLog（SPSC ring → 寫檔執行緒）；GatewayMetrics 計數
```

設計原則：

- **單一執行緒擁有所有狀態**：gateway、session、order table、risk 只在 gateway 執行緒上修改，hot path 不加鎖。
  其他執行緒只透過 SPSC 佇列（控制指令、audit）或單一 atomic（kill switch）互動。
- **Session 不碰 I/O**：吃 bytes、吐 bytes、時間由外部傳入，所以同一份程式能跑在 TCP、記憶體內管線（測試）或之後的 kernel bypass 上。
- **先送出、再記帳**：bytes 先交給 transport，之後才寫 message store、audit log、計數器。

## 2. 模組

| 檔案 | 內容 |
|---|---|
| `gw/instrument.hpp` | 商品表（代號 ↔ index、每手股數、可賣空、限制名單、參考價、交易時段）、HKEX 價位表 |
| `gw/order_state.hpp` | 委託狀態表（§3） |
| `gw/risk.hpp` | 下單前風控、kill switch |
| `gw/rate_limit.hpp` | GCRA（token bucket）、滑動視窗 |
| `gw/throttle.hpp` | 交易所流量控制：撤單 > 改單 > 新單的優先佇列 |
| `gw/spsc.hpp` | SPSC 佇列與位元組環 |
| `gw/audit.hpp` | 稽核 log（背景寫檔）與讀取 |
| `gw/metrics.hpp` | 計數器、控制通道 |
| `gw/net/tcp.hpp` | 非阻塞 TCP（TCP_NODELAY、可選 SO_BUSY_POLL） |
| `gw/ocgc/protocol.hpp` | OCG-C 欄位表、Writer / Reader、checksum |
| `gw/ocgc/crc32c.hpp` | CRC32C 四種實作 + 增量 shift |
| `gw/ocgc/order_template.hpp` | New Order 模板 |
| `gw/ocgc/messages.hpp` | 其餘訊息的編解碼（session、Execution Report、Cancel / Amend / Mass Cancel、Lookup） |
| `gw/ocgc/session.hpp` | Session 狀態機 |
| `gw/ocgc/password.hpp` | Logon 密碼 RSA 加密（OpenSSL） |
| `gw/ocgc/report_adapter.hpp` | Execution Report → 中立的 `Report` |
| `gw/ocgc/gateway.hpp` | `OcgcGateway`、`SessionRouter` |
| `gw/ocgc/connector.hpp` | Lookup、primary / mirror 切換、重連 |
| `gw/ocgc/reconcile.hpp` | 從 audit log 重建成交、與外部紀錄對帳 |
| `gw/ocgc/sim/exchange.hpp` | 交易所模擬器（server 端 session + 撮合） |
| `gw/ocgc/sim/server.hpp` | 模擬器的 TCP 前端（Lookup、primary、mirror 三個 port） |

工具：`obl_ocgc_demo`（整套跑在 loopback TCP 上）、`obl_ocgc_audit`（dump / fills / reconcile）、`obl_ocgc_bench`。

## 3. 委託狀態表

- **Request ID 直接當陣列索引**：每個請求（新單、撤單、改單、全撤）都配一個新的 Client Order ID（規格要求每日唯一），
  由 gateway 從 10,000,000 起連續配發，`id − 10,000,000` 就是 request 表的索引 → order slot。送單與收回報都不用 hash。
  從 10,000,000 起配發也讓每個 ID 剛好 8 位數（規格：1–99,999,999、不可前導零）。
- **一張單同時只有一個請求在途**（撤單或改單）。OCG-C 對 pending 中的單再送撤單會拒絕（Cancel Reject Code 3），所以在本地就擋下。
- **64 bytes 的 Order**（一條 cache line）放送單與回報都會碰的欄位；交易所 Order ID、strategy tag、route、per-symbol 鏈結串列放另一個陣列。
- **最壞情況曝險**：每張活單的未成交量 = leaves；改單在途時取 max(原 leaves, 新 leaves)。
  每次狀態變化都「先扣掉舊貢獻、改狀態、加回新貢獻」，per-symbol 與全帳戶的 open qty / notional 永遠等於所有活單加總，
  不必重掃（隨機測試每 97 步就從頭重算比對一次）。
- **重複回報**：PossResend 的回報可能用新序號再來一次。成交用累計成交量去重（CumQty 沒有增加就是舊的），其他回報本身就是冪等的。
- **例外**：撤單途中成交、成交後才到的撤單拒絕、改單途中成交、交易所主動撤單、取消成交（Trade Cancel 會把部位改回來）、
  已結束的單又來一筆成交（仍計入部位：錢是真的）。

## 4. 功能完成度（對照規劃 §2）

| 規劃項目 | 狀態 | 說明 |
|---|:---:|---|
| **2.1 Session** | | |
| 登入 / 登出 / heartbeat / test request / 斷線偵測 | ✅ | 20 s heartbeat；60 s 無訊息送 Test Request，再 60 s 無回應登出 |
| 序號管理與斷線恢復 | ✅ | 雙向序號跨重連延續；收方向跳號送一次 Resend Request；§5.3 兩種 Logon 恢復情境；PossDup 處理 |
| 斷線時新單直接拒絕、排隊中的請求全部丟棄並通知 | ✅ | `RejectedSessionDown`；`DropReason::SessionDown` |
| 漏送的業務訊息：重播或作廢 | ✅ | `ReplayPolicy::Replay`（照規格）/ `GapFillBusiness`（作廢並通知 strategy） |
| 多 session | ✅ | 多個 gateway 共用 order table 與 risk；`SessionRouter` 依 throttle 剩餘量分配新單，撤改單走原 session |
| Primary / backup 切換 | ✅ | 4 個 Lookup endpoint 依序；primary → mirror → Lookup；斷線等 10 s |
| Lookup Service | ✅ | Lookup Request / Response |
| Logon 密碼加密 | ✅ | UTC 時間前綴 + RSA-2048（PKCS#1 / OAEP）+ base64；每次登入重新產生 |
| Cancel-on-disconnect | ⚠️ | 交易所端功能（§6.11）：依 Comp ID 向 HKEX 申請、當日不能改，可設延遲（延遲內重連就不撤）；協定沒有欄位，gateway 只需處理重連後收到的撤單回報（已涵蓋） |
| 交易時段 | ✅ | `MarketPhase`：競價時段只接受 at-crossing；連續交易不接受 at-crossing；停牌 / 收市全拒（時段由行情端設定） |
| **2.2 編解碼** | | |
| New / Cancel / Amend / Mass Cancel | ✅ | New Order 用模板，其餘用 Writer |
| 各種回報 | ✅ | Execution Report 全部 Exec Type、Business Message Reject、Reject、Mass Cancel Report |
| 單型 | ✅ | 限價；TIF Day / IOC / FOK / At Crossing；賣空（Side 5）；Order Capacity |
| Market order、post-only、iceberg | ❌ | HKEX 證券市場沒有 post-only / iceberg；market（Order Type 1）只用在競價時段，尚未接 |
| 模板 + 增量 CRC | ✅ | 見 `ocgc_binary.md` |
| **2.3 委託狀態** | ✅ | §3 |
| **2.4 風控** | | |
| 單筆數量 / 金額上限 | ✅ | |
| 價格偏離（collar） | ✅ | 基點；沒有參考價時拒單 |
| 價位、每手股數 | ✅ | HKEX 價位表、board lot |
| 持倉上限（含在途） | ✅ | long：部位 + 在途買單；short：部位 − 在途賣單 |
| 帳戶額度 | ✅ | 全帳戶在途名目金額（incremental，O(1)） |
| 委託速率 | ✅ | GCRA；多次超限觸發 kill switch |
| 重複單 | ✅ | 同商品 / 方向 / 價 / 量在時間窗內 |
| Self-trade prevention | ✅ | 新單或改單會撞到自己的掛單就拒絕（掃該商品的活單） |
| 限制名單、賣空 | ✅ | 限制名單；賣空需帳戶允許且在可賣空名單；一般賣單不得超過部位減在途賣單 |
| Kill switch | ✅ | 手動或自動（單日虧損、交易所拒單率、速率超限）；封鎖新單與改單、丟棄排隊、Mass Cancel（被拒則逐筆撤）；斷線期間觸發則重連後補撤；只能人工解除 |
| **2.5 Throttle** | ✅ | 1 秒滑動視窗（不知道交易所固定視窗的起點，滑動視窗在任何對齊下都不會超量）；撤單 > 改單 > 新單；排隊中的改單可直接改目標價量；新單排太久就作廢 |
| **2.6 傳輸** | | |
| Kernel TCP、TCP_NODELAY、busy poll | ✅ | 非阻塞 socket；`SO_BUSY_POLL` 可選 |
| Kernel bypass、FPGA | ❌ | 沒有硬體；Session 不碰 I/O，之後可換 transport |
| **2.7 觀測與營運** | | |
| 稽核 log | ✅ | 每則進出訊息 + 時間戳；SPSC ring + 背景執行緒寫檔；滿了預設等待（不丟） |
| 對帳 | ✅ | 從 audit log 重建成交（依 Execution ID 去重、處理 Trade Cancel），與外部 CSV 比對成交與部位 |
| Drop copy session | ❌ | OCG-C Drop Copy 是另一份規格，未實作 |
| 監控計數 | ✅ | 送出 / 回報 / 拒單（依風控原因）/ 排隊 / 丟棄 / session 上下線 / 進出訊息數 |
| 設定熱更新 | ✅ | `ControlChannel`：其他執行緒送閉包，gateway 在 `poll()` 中執行 |
| 延遲時間戳與分佈 | ⏳ | 下一階段（量延遲） |
| **2.8 測試** | | |
| 交易所模擬器 | ✅ | server 端 session、價格時間優先撮合、IOC / FOK、改單優先權規則、Mass Cancel、throttle 拒單 |
| 故障注入 | ✅ | 丟回報、壞 checksum、拒絕登入、交易所登出、primary 掛掉 |
| Fuzzing | ✅ | 30 萬筆變異訊息（重算 CRC 讓它進到 parser）+ 隨機序號 / 切塊的 session 串流，ASan / UBSan 下無錯 |
| 真實行情驅動的端到端 | ⏳ | 下一階段（tick-to-trade） |

## 5. 與規格的差異、待確認

- **GapFillBusiness**：§5.6 要求業務訊息要重播；用 gap fill 作廢新單是否被 OCG-C 接受，需向 HKEX 確認。預設是照規格重播。
- **Mass Cancel 的範圍是 Broker ID**（§6.6.6：「All orders to be mass cancelled here must belong to the given Broker ID」）：
  多個 session 共用 Broker ID 時，任一 session 的 Mass Cancel 會撤掉所有 session 的單。kill switch 會從每個 session 各送一次，
  第一個撤掉全部，之後的 Mass Cancel 不會再撤到任何單。模擬器目前只撤送出 Mass Cancel 那個 Comp ID 的單，與實際行為不同。
- **Business Message Reject**：視為「請求沒被接受」：新單變成 Rejected，撤單 / 改單恢復成沒有在途請求。
- **未實作的訊息**：Quote（16–18）、Trade Capture（21、22）、OBO Cancel（23、24）、Throttle Entitlement（25、26，含 repeating block）、
  Party Entitlement。報價與場外成交不是這個 gateway 的範圍；throttle 上限目前手動設定。
- **時間**：`now` 是 UTC 奈秒（CLOCK_REALTIME），Transaction Time 需要牆上時間；若系統時鐘被調整，計時器的差值也會受影響。
- **模擬器**：自己的簡化撮合（`std::map` + `std::list`），不是 OTP-C 的行為模型，也沒有用本 repo 的 L3 book（它是為行情設計的，不負責撮合）。

## 6. 執行

```bash
cmake -S . -B build && cmake --build build -j
cd build && ctest                      # 13 組測試
./build/obl_ocgc_demo audit.bin        # 整套走 loopback TCP，見下方輸出
./build/obl_ocgc_audit dump audit.bin
./build/obl_ocgc_audit fills audit.bin > fills.csv
./build/obl_ocgc_audit reconcile audit.bin fills.csv
```

`obl_ocgc_demo` 的流程：經 Lookup 登入 → 掛單 → 與外部流動性成交 → 改單 → 6 筆連發（本地限速 5/s，4 筆排隊、約 1 秒後送出）
→ 價格偏離被風控擋下 → primary 掛掉、切到 mirror（序號延續、委託狀態不變）→ kill switch（1 則 Mass Cancel、8 筆撤單回報）→ 登出。

測試：

| 測試 | 範圍 |
|---|---|
| `test_ocgc` | CRC32C、各訊息編解碼、presence map、模板 |
| `test_ocgc_session` | Session 狀態機對腳本化的對手 |
| `test_order_state` | 委託狀態表，含隨機一致性測試 |
| `test_risk` | 每一項風控、kill switch 觸發 |
| `test_throttle` | 滑動視窗、優先佇列、作廢 |
| `test_gateway` | 記憶體內端到端：gateway ↔ 模擬器，含重連、恢復、多 session、audit、對帳、控制通道 |
| `test_tcp` | 真 TCP：Lookup 換 endpoint、RSA 登入、primary → mirror |
| `test_spsc` | 兩個執行緒的 SPSC（ThreadSanitizer 下無錯） |
| `test_fuzz` | 解碼器與 session 的 fuzzing |

## 7. 下一步：量延遲

依規劃 §3 的 G1–G9：送單路徑分段時間戳（strategy 決策 → risk → order table → throttle → 編碼 → `send()`）、
回報路徑（收到 → 解碼 → 狀態更新 → strategy）、loopback TCP 往返、warm-up 效果，以及用 ITCH / XDP 行情驅動的 tick-to-trade。
