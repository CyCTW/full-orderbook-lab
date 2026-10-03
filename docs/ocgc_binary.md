# HKEX OCG-C Binary Trading Protocol：New Order 格式與 checksum 成本

規格來源：HKEX《Interface Specifications – HKEX Orion Central Gateway – Securities Market, Binary Trading Protocol》
v3.2（2023-07-19，加入 Self-Match Prevention）。OCG-C 官方頁面目前掛的是 v3.1；v3.2 在
[Self-Match Prevention 頁面](https://www.hkex.com.hk/-/media/HKEX-Market/Services/Trading/Securities/Overview/Trading-Mechanism/Self-Match-Prevention/HKEX_OCGC_Binary_Trading_Interface_Specifications_v3_2-(Markedup).pdf)。
以下 §x.y 皆指該文件章節。

程式：`include/obl/gw/ocgc/`（`protocol.hpp` 欄位表與 Writer/Reader、`crc32c.hpp`、`order_template.hpp`、
`messages.hpp` session 訊息與 Execution Report、`session.hpp` session 狀態機），
測試 `tests/test_ocgc.cpp`、`tests/test_ocgc_session.cpp`，benchmark `bench/ocgc_bench.cpp`。

## 1. 訊息結構

所有整數 little-endian（與 OMD-C 相同）。訊息 = header + body + trailer（§6.2、§7.2、§7.3）：

| offset | bytes | 欄位 | 型別 / 說明 |
|---:|---:|---|---|
| 0 | 1 | Start of Message | UInt8，固定 STX `0x02` |
| 1 | 2 | Length | UInt16，**整則訊息**長度（含 header、trailer） |
| 3 | 1 | Message Type | UInt8，New Order = 11、Amend = 12、Cancel = 13、Mass Cancel = 14、Execution Report = 10 |
| 4 | 4 | Sequence Number | UInt32，每日從 1 開始，雙向各自獨立（§4.2） |
| 8 | 1 | PossDup | UInt8 |
| 9 | 1 | PossResend | UInt8 |
| 10 | 12 | Comp ID | Alphanumeric(12) |
| 22 | 32 | Body Fields Presence Map | 256 bits；**bit 0 = byte 0 的 MSB** |
| 54 | — | body | presence map 中為 1 的欄位，依 bit 順序緊密排列 |
| n−4 | 4 | Checksum | UInt32，CRC32C（下節） |

型別（§6.1）：

- **Alphanumeric(n)**：固定 n bytes，NUL 結尾且 NUL 算在 n 內，NUL 之後的內容忽略
- **Alphanumeric 變長**（只有 Text）：前 2 bytes UInt16 長度，再接內容
- **Decimal**：Int64，**8 位隱含小數**；價格與**數量**都是 Decimal（100 股 = `100 × 10^8`）

特點：雖然是 binary 協定，但欄位語意沿用 FIX 5.0 SP2，且許多欄位是**定長 ASCII**
（Client Order ID、Transaction Time、Broker ID…），所以送單時仍有「數字轉字串」的成本。

## 2. New Order (11) — 限價、Day、只帶必要欄位

§7.6.1 列出 bit 0–23 的欄位，必要欄位與實際 layout（`tests/test_ocgc.cpp` 逐 byte 驗證）：

| bit | 欄位 | 型別 | offset | 每筆單會變？ |
|---:|---|---|---:|:---:|
| — | header | | 0 | Sequence Number |
| 0 | Client Order ID | Alnum(21) | 54 | ✅ |
| 1 | Submitting Broker ID | Alnum(12) | 75 | |
| 2 | Security ID | Alnum(21)，如 `"700"` | 87 | |
| 3 | Security ID Source | UInt8 = 8 (Exchange Symbol) | 108 | |
| 4 | Security Exchange | Alnum(5) = `"XHKG"`（Source = 8 時必填） | 109 | |
| 6 | Transaction Time | Alnum(25)，`YYYYMMDD-HH:MM:SS.ssssss`，UTC | 114 | ✅（日期部分每日固定） |
| 7 | Side | UInt8：1 Buy / 2 Sell / 5 Sell Short | 139 | |
| 8 | Order Type | UInt8：1 Market / 2 Limit | 140 | |
| 9 | Price | Decimal | 141 | ✅ |
| 10 | Order Quantity | Decimal | 149 | ✅ |
| 18 | Disclosure Instructions | UInt16，bit 0 = 1（無須揭露） | 157 | |
| 22 | Submitting BCAN Field | Alnum(21)，`CE號.BCAN`，例 `ABC123.2568` | 159 | |
| — | Checksum | UInt32 | 180 | ✅ |

**總長 184 bytes**，presence map 前三個 byte 為 `FB E0 22`。選填欄位：TIF（bit 11，缺省 = Day；IOC = 3、FOK = 4）、
Broker Location ID(11)、Order Capacity、Max Price Levels、Text、SMP ID(10)… 每加一個長度就跟著變。

**Client Order ID 限制**（§6.6.3.1）：目前只能是數字 1–99,999,999，不可有前導零，每交易日內唯一。
實作上從 10,000,000 開始配發，ID 永遠剛好 8 位數，送單時不必處理長度變化。

## 3. Checksum：CRC32C

§4.8：**CRC32C（Castagnoli，polynomial 0x1EDC6F41）**，範圍是 header + body（不含 checksum 本身），
UInt32 放在最後。OCG-C 驗證失敗會**直接斷線、不送 Logout**；收到 OCG-C 訊息驗證失敗也應斷線。

CRC32C 正好是 x86 SSE4.2 `crc32` 指令的多項式，可以硬體計算。

## 4. 量測結果

環境：Intel Xeon 2.8 GHz（雲端 VM，4 vCPU），GCC，`-O3 -march=native`，綁單核。
「串接」= 每次輸入依賴上一次結果，無法重疊執行，反映送單路徑實付的延遲；
「單次 p50/p99」= 每次呼叫前後用 rdtsc 量並扣掉計時本身的成本。三次執行差異 < 10%
（原始輸出 `results/ocgc/2026-10-03_encode_crc_run{1,2,3}.txt`）。

### 4.1 只算 checksum（180 bytes）

| 方法 | 串接 ns | 單次 p50 | 單次 p99 |
|---|---:|---:|---:|
| 逐 bit | 1,870 | 1,800 | 2,000+ |
| Sarwate（1 張表，每次 1 byte） | 450 | 438 | 455 |
| slice-by-8（8 張表，8 KiB） | 120 | 113 | 190–230 |
| **SSE4.2 `crc32`（每指令 8 bytes）** | **27.5** | **27.5** | 28–51 |
| **增量（只算 4 段會變的 span + pclmul）** | **12.6** | **13.6** | 21 |

硬體版：180 bytes = 22 個 `crc32 u64` + 1 個 `u32`，每個 latency 3 cycles 且前後相依，
23 × 3 = 69 cycles ≈ 25 ns，與量到的 27.5 ns 吻合。**時間與長度成正比**（32 B → 10 ns，1 KiB → 125 ns）。

### 4.2 整則 New Order（填欄位 + checksum）

| 方法 | 串接 ns | 單次 p50 | 單次 p99 |
|---|---:|---:|---:|
| 從頭組 + `snprintf` 轉字串 + 硬體 CRC | 250–275 | 245–275 | 490–560 |
| 從頭組 + 查表轉字串 + 硬體 CRC | 43–51 | 50–54 | 67–89 |
| 模板，只改欄位（不算 CRC） | 10–20 | 18 | 31–37 |
| 模板 + slice-by-8 | 125 | 121 | 139–205 |
| 模板 + 硬體 CRC | 28–32 | 36–37 | 41–57 |
| 模板 + 增量 CRC（寫回記憶體後重讀） | 18–19 | 31–36 | 37–119 |
| **模板 + 增量 CRC（欄位在暫存器組好）** | **17** | **25** | **30–38** |

### 4.3 觀察

1. **CRC 演算法選錯比什麼都貴**：查表版 120–450 ns，是整個送單路徑預算的好幾倍；硬體指令 27 ns。
2. **`snprintf` 是第二大成本**：光兩個欄位（ClOrdID、Transaction Time）就 ~200 ns。換成兩位數查表後整則 50 ns。
3. **模板**：Broker、Security、BCAN、presence map 等每筆不變的欄位預先寫好，只改 5 個欄位，比從頭組快 ~13–20 ns。
4. **增量 CRC**：CRC 在固定長度下是仿射的，模板把會變的 span 留為 0，
   `crc(訊息) = crc(模板) ⊕ Σ shift(crc(span), span 之後的 bytes 數)`。
   4 段 span（seq 4 B、ClOrdID 8 B、時間 16 B、價量 16 B）各自是一條短的相依鏈，可以平行執行；
   「補零 k 個 byte」用一個 `pclmulqdq` + 一個 `crc32` 完成（常數 `x^(8k−33) mod P` 在建模板時算好）。
   checksum 本身 27.5 → 13.6 ns。
5. **store-forwarding**：先用 2-byte store 寫數字、再用 8-byte load 讀回來算 CRC，load 無法從 store buffer 轉送，
   要等 store 寫入 cache，p99 明顯變差（最多 119 ns）。改成在暫存器組好 8-byte word、存一次、直接拿暫存器算 CRC 後，
   p50 31 → 25 ns，p99.9 從 ~170 ns 降到 42–77 ns。
6. 結論：**OCG-C New Order 編碼 + checksum 可以壓在 ~25 ns（p50）**，其中 checksum 約一半。
   最直覺的寫法（snprintf + 硬體 CRC）245 ns，約是最佳化版本的 10 倍；若 CRC 再用逐 byte 查表，估計約 700 ns。

p99.9 普遍有 ~170 ns 的尾巴，在多數變體都出現，推測是 VM 的中斷 / 計時干擾，不是程式本身。

## 5. Execution Report 與接收路徑

### 5.1 格式

Execution Report (10) 所有種類（接受、拒絕、撤單、改單、成交、成交取消…）共用一張 bit 表（§7.6.7），
出現哪些欄位由 Exec Type 決定。常用欄位：

| bit | 欄位 | 型別 |
|---:|---|---|
| 0 / 8 / 9 | Client Order ID / Original Client Order ID / Order ID | Alnum(21) |
| 21 | Execution ID（每則回報唯一，用來去重） | Alnum(21) |
| 22 | Order Status：0 New、1 部分成交、2 全部成交、4 已撤、8 拒絕、12 過期… | UInt8 |
| 23 | Exec Type：`'0'` New、`'4'` Cancel、`'5'` Amend、`'8'` Reject、`'F'` Trade、`'H'` Trade Cancel、`'X'`/`'Y'` 撤單/改單被拒 | Byte（ASCII） |
| 24 / 25 | Cumulative / Leaves Quantity | Decimal |
| 26 / 29 / 36 | 拒絕原因碼（新單 / 撤單 / 改單） | UInt16 |
| 32 / 33 | 這次成交的數量 / 價格 | Decimal |
| 38 / 42 | Trade Match ID / Aggressor Indicator | Alnum(25) / UInt8 |

一則典型的部分成交回報 264 bytes。presence map 出現未定義的 bit（例如 37）時無法得知欄位長度，
解碼直接失敗，不會猜長度往下讀。

### 5.2 量測（264 bytes 成交回報，3 次執行；`results/ocgc/2026-10-03_session_run{1,2,3}.txt`）

| 步驟 | 單次 p50 | 單次 p99 |
|---|---:|---:|
| 只驗 CRC | 36 ns | 39–65 |
| 走訪欄位：逐一測 256 個 bit | 240 ns | 420–460 |
| 走訪欄位：4 個 64-bit word + count-leading-zeros | 49 ns | 50–61 |
| `decode_exec_report`（欄位延遲量長度） | 53 ns | 59–78 |
| 驗 CRC + 解碼 + ClOrdID 轉數字 | 75 ns | 119–158 |
| **`Session::on_bytes` 整條**（切 frame、CRC、序號檢查、分派、解碼） | **81 ns** | 131–192 |

觀察：

1. **presence map 不要逐 bit 測**：一則回報只有 ~20 個欄位，但逐 bit 要跑 256 次迴圈與分支（240 ns）。
   改成把 32 bytes 當 4 個 big-endian 64-bit word，用 `countl_zero` 直接跳到下一個 1，降到 49 ns。
2. **字串欄位延後量長度**：Alnum 欄位要找 NUL 才知道長度，每個欄位一次 `memchr`。
   解碼時只記指標與容量（`LazyStr`），用到才找 NUL，解碼 68 → 53 ns。實際上策略通常只看 ClOrdID 與數量。
3. 接收端 CRC 沒辦法用增量技巧（每則內容都是新的），264 bytes 就是 ~36 ns，佔整條接收路徑近一半。

## 6. Session 層

`include/obl/gw/ocgc/session.hpp`。不碰 socket、不讀時鐘、不在送單路徑配置記憶體：
外部餵進收到的 bytes、連線事件與時間，它透過 Transport 送出、透過 Handler 回報。
同一份程式可以接 TCP、kernel bypass 或測試用的假對手（`tests/test_ocgc_session.cpp`）。

### 6.1 狀態與規則（§4、§5）

```
Disconnected --on_connected--> LogonSent --收到 Logon(Status 0/1)--> Active --logout()--> LogoutSent
     ^                             |  60 s 無回應 / 收到 Logout、Reject            |  收到 Logout 回覆 / 60 s
     +-----------------------------+------------------------------------------------+
```

| 情況 | 處理 |
|---|---|
| 序號 | 雙向各自從 1 開始、跨重連延續，每天重設（`reset_sequences`） |
| LogonSent 期間送單 | 直接回 false（§4.1：送了會被斷線） |
| 收到序號 > 預期 | 送一次 Resend Request(預期, 0)，亂序訊息丟掉等重播；追上前不再送第二次 |
| 收到序號 < 預期 | PossDup = 1 就忽略，否則送 Logout 並斷線 |
| 收到 Logon 回覆 | **不因 Logon 的序號送 Resend Request**（§5.3）：OCG-C 會從我們的 Next Expected 開始重播，並用 gap fill 跳過它自己 Logon 的序號 |
| Logon 回覆的 Next Expected < 我們的下一個序號 | 我們照樣重播，並 gap fill 掉自己的 Logon |
| Next Expected > 我們送過的 | 送 Logout 並斷線（§5.3：需人工處理） |
| 對方 Resend Request | Session 層訊息（Logon、Heartbeat…）用 Sequence Reset gap fill 跳過；業務訊息依 ReplayPolicy |
| 20 s 沒送東西 | 送 Heartbeat |
| 60 s 沒收到東西 | 送 Test Request；再 60 s 沒回應 → Logout 並斷線 |
| CRC 錯誤 | 不送 Logout，直接斷線（§4.8） |

### 6.2 重播業務訊息：規格 vs 我們之前討論的做法

§5.6 規定：除了 Session 層訊息以外，「所有其他訊息都應該重播」。也就是說，OCG-C 沒收到的 New Order，
client 被要求在重連後**補送**。這跟先前的討論（過時的單應該回拒絕給策略、不要晚送）衝突，所以做成可設定：

- `ReplayPolicy::Replay`（預設，照規格）：業務訊息以原序號、PossDup = 1 重播
- `ReplayPolicy::GapFillBusiness`：業務訊息也用 gap fill 跳過，並對每一筆呼叫 `Handler::on_not_sent`，
  讓策略知道這張單從未到達交易所

GapFillBusiness 是否被 OCG-C 接受，規格沒有明說（只說「expected to be replayed」），上線前要跟 HKEX 確認。

注意 PossResend：OCG-C 斷線期間產生的回報，重連後可能用**新序號**加 PossResend = 1 再送一次（§5.5.2、§5.7），
所以 Handler 要用 Execution ID 去重，Session 層看序號是分辨不出來的。

### 6.3 送單路徑上的 message store

重播需要保留今天送出的每一則訊息。`send_new_order` 先把 bytes 交給 Transport，**之後**才複製進 store，
store 的成本不會延後封包送出，但會延後下一筆單。

| `Session::send_new_order`（fill_regs + send + store） | 單次 p50 | p99 | p99.9 |
|---|---:|---:|---:|
| store 只 `reserve()`，記憶體還沒碰過 | 31 ns | **1,616–1,753 ns** | 3.0–3.1 µs |
| **store 啟動時先把每一頁寫過一次（prefault）** | 30–39 ns | **216–265 ns** | 485–548 ns |

`reserve()` 只保留位址空間，第一次寫入每個 4 KiB 頁面時才觸發 page fault（約每 22 筆單一次，每次 µs 等級）。
啟動時先寫過一遍（`SessionConfig::prefault_store`，預設開）後 p99 降到約 1/7。剩下約 220 ns 的 p99，
推測是寫入從沒碰過的 cache line 所造成的 cache miss（store 一直往新記憶體寫），之後可以試 non-temporal store
或改成重複使用的環狀緩衝區。

### 6.4 尚未處理

- Lookup Service（先問 IP / port）與 RSA 密碼加密：目前 `encrypted_password` 由外部提供
- Throttle：超過每秒訊息上限時 OCG-C 回 Business Message Reject（§6.12），還沒有本地端的流量控制
- 委託狀態表與 ExecID 去重：目前只把回報交給 Handler

## 7. 下一步

- 委託狀態表（ClOrdID 當陣列索引）+ Execution Report 套用（G2）
- 模擬 OCG-C：用 L3 book 撮合，走 loopback TCP，量送單 → 回報的往返時間
- Throttle（token bucket，撤單優先）
- warm-up：送單很稀疏時，模板、CRC 常數與 session 狀態是否還在 cache
