# N24 輸出 API（WASAPI / ASIO / Core Audio）

`core/include/daphne/engine/audio_output.h`、`wasapi_output.h`、`asio_output.h`、`coreaudio_output.h`、`devices.h`，以及對應的 `core/src/*.cpp`

## 共同介面（`AudioOutput`）

- `open` 協商格式（不開始播放）。
- `start(ring, eof)` 開始從環讀取：eof 設定且環讀空時 `drained()` 變真，裝置之後收到靜音直到 `stop()`。
- `format()` 是裝置實際鎖定的格式，`matches_request()` 表示是否等於要求。
- `rearm()`：串流結束後又搜尋回來時解除 drained。
- `failed()` / `failure()`：裝置在播放中失效（拔除、被其他程式獨占）。
- `take_notes()`：驅動訊息、取樣率變更、遲到的回呼，交給主程式的記錄。
- `reset_requested()`：驅動要求重新開啟（播放器會重開並在原位置繼續）。
- 暫停 / DoP / 電平委派給 N23 的送料。
- `probe_before_design()`：有些驅動接受了串流不了的取樣率，播放器先以靜音短暫啟動一次再設計濾波器（ASIO 為 true）。
- `supported_rates()`：開啟中的驅動接受的 PCM 取樣率，給主程式的設定畫面（ASIO）。
- `buffer_fallback()`：裝置不接受 `output.buffer_ms`，以預設緩衝執行（WASAPI 獨占）。
- `system_rate()`：輸出把串流轉成裝置自己的取樣率時，裝置的取樣率（Core Audio 的 `AudioConverter`、WASAPI 共享的 `AUTOCONVERTPCM`）；`format().rate` 是播放器送出的取樣率。0 表示裝置就以 `format().rate` 播放。
- 播放器只透過這個介面使用裝置：裝置由 `PlayerDeps::open_output` 建立，Windows 預設為 `platform_output_opener()`（`core/src/output_win.cpp`：WASAPI / ASIO），macOS 為 `core/src/output_mac.cpp`（Core Audio）；Linux 目前沒有裝置輸出（N25）。
- 開裝置之前需要知道的三件事由 `PlayerDeps::output_caps`（`OutputCaps`，預設 `platform_output_caps()`）回答，播放器不看 API 名稱：`native_dsd`（直接收 DSD：ASIO）、`shared_mix`（系統混音器、自己的取樣率，不會 bit-perfect、不送 DSD 與 DoP：WASAPI 共享）、`takes_buffer_ms`（`output.buffer_ms` 決定緩衝：ASIO 以外）、`converts`（`OutputRequest::convert` 時可以轉成裝置有的取樣率：Core Audio 獨佔；播放器只在最後一次 PCM 嘗試要求，DoP 從不要求）。Core Audio：沒有原生 DSD（DoP 只在獨佔）、共享模式是 `shared_mix`、吃 `buffer_ms`、獨佔可以轉換。
- `output.api` 的預設是這個平台的獨佔輸出（`kDefaultOutputApi`：Windows `wasapi_exclusive`、macOS `coreaudio_exclusive`）；別的平台的 API 開啟時回報原因。CLI 的 `--shared` / `--exclusive` 也對應到這個平台的 API。

統計（`OutputStats`，原子變數）：回呼數、underrun、WASAPI 事件逾時（兩個緩衝長度、至少 50 ms 沒有裝置事件）、最長回呼間隔、ASIO 遲到回呼（相鄰回呼間隔 > 1.5 個緩衝區週期）。

## WASAPI

| 模式 | 做法 |
|---|---|
| 獨占 | 事件驅動（`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`）。格式搜尋順序：取樣率為要求值、同家族逐次減半，再試 192k / 96k / 48k / 44.1k；樣本格式為 int32、24-in-32、float32、packed24、int16。每種組合先以 `IsFormatSupported` 檢查 |
| 共享 | 系統混音器格式；取樣率不同時請 Windows 轉換（`AUTOCONVERTPCM`），以正確速度播放（共享模式本來就不是 bit-perfect）。這時 `system_rate()` 是混音器的取樣率，報告與 MIKU 的訊號路徑以它顯示實際輸出 |

- 緩衝長度：`output.buffer_ms`，0 為預設（獨占：裝置週期、至少 25 ms，一個緩衝等於一個週期；共享：100 ms），否則 10–200 ms（1–9 視為 10）。
  - 下限 10 ms：Windows 獨占模式常見的預設週期；更短的週期少有裝置接受，對播放也沒有好處（播放器本身預先處理約 0.5 秒，裝置緩衝只影響暫停與搜尋的反應速度）。
  - 上限 200 ms：下面 VB Hi-Fi Cable 的情形 50 ms 已經解決，200 ms 留 4 倍餘量；再長的話，暫停與搜尋都要等一整個緩衝才生效。
  - 搜尋時等裝置淡出的上限隨緩衝調整（兩個緩衝加 50 ms，至少 300 ms）：裝置下一次回呼才淡出，固定 300 ms 在長緩衝下會在淡出前就清空環形緩衝。
  - 時鐘粗糙的裝置需要較長的緩衝：VB Hi-Fi Cable 以 15.6 ms 的系統計時器計時（事件間隔 31 / 31 / 16 ms 輪流），25 ms 緩衝偶爾來不及，裝置端補幾 ms 的零（約 4 次 1 次；10 ms 每次都有，50 ms 16 次都沒有）。播放器的 underrun 計數看不到這種情形（環形緩衝始終有資料）。裝置不接受指定的長度時退回預設，並記錄一則訊息。MIKU 的「緩衝大小」（40–200 ms）對應這個設定。
- 緩衝區未對齊（`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`）時，以裝置回報的對齊大小重新初始化。
- 開始前先送一個緩衝區的靜音（pre-roll）。
- 裝置失效（`AUDCLNT_E_DEVICE_INVALIDATED`）→ `failed()`。
- 鎖定的格式與要求不同時，呼叫端看得到（播放器會依此重試或報錯，絕不以錯誤的速度播放）。
- 每次開啟各自初始化 COM（MTA），關閉時解除。開啟失敗時先釋放手上的介面（裝置列舉器、屬性存放區）再解除：在 COM 只由這次開啟初始化的執行緒上，解除之後才 Release 會當機。

## ASIO

- 開啟流程：init（桌面視窗）→ 輸出聲道限制在硬體數量（多出的由處理鏈降混）→ `kAsioSetIoFormat`（PCM 或 DSD）→ setSampleRate，之後最多讀回 3 次、每次間隔 5 ms 並重新設定 → 偏好緩衝大小（驅動沒給時用 512）→ createBuffers。
- 驅動以 CoCreateInstance 建立，失敗時直接載入 InprocServer32 的 DLL。
- 裝置取樣率是驅動的讀回值：硬體沒有鎖定要求的取樣率時，呼叫端看得到。
- 每一種 ASIO PCM 樣本型態都正確寫入：Int32LSB16..24 右對齊，MSB 型態交換位元組。
- 驅動要求重設或改變緩衝大小時重新開啟裝置（播放器在原位置繼續）。啟動後 0.5 s 內的一次會被忽略（驅動啟動時常會送一次）。10 秒內超過 3 次視為失敗。取樣率改變則停止播放並說明原因。
- 驅動可能在自己的音訊執行緒呼叫 asioMessage / sampleRateDidChange，所以這兩個回呼不配置記憶體、不阻塞：記錄訊息以 `vsnprintf` 寫進預先配置的固定欄位（`try_lock` 取得；播放器正在讀取或已滿 64 則時捨棄並計數，下次 `take_notes()` 回報捨棄幾則），失敗原因寫進固定緩衝區後才以 release 設定 `failed()`，延遲是原子變數。
- 驅動 DLL 永不卸載：經由 COM 釋放時會在 CoUninitialize 卸載，若驅動的執行緒還在收尾，就會在已釋放的程式碼中執行而當機。
- DSD 模式：環不夠時以 DSD 靜音（0x69）填補，不是 0。
- 原生 DSD 結束時不切回 PCM（有些 DAC 在原生 DSD → PCM 的切換會爆音，而且關閉時跑一段 PCM 工作階段後，下一次原生 DSD 可能沒有聲音）：DAC 留在 DSD 模式，下一個 PCM 或 DoP 串流在自己的取樣率先送 0.2 s 全為 0 的 PCM，讓 DAC 離開 DSD 模式（有些 DAC 在原生 DSD 之後直接接 DoP 會在某一聲道持續發出嗶聲）。同一個程序裡的第一個 PCM / DoP 串流也這樣開始（不知道 DAC 之前停在什麼模式）。
- 開始前兩個 buffer 都先填靜音（PCM 為 0、DSD 為 0x69），驅動程式在第一次 callback 之前播放的那一段不會是 createBuffers 留下的內容。
- 原生 DSD 在播完之前被停止時，先讓 DSD 靜音（0x69）送到 DAC 再停止，不在音樂中間切斷。
- 需要建置時有 Steinberg ASIO SDK（`third_party/asiosdk`，`scripts/fetch_asiosdk.ps1`）；沒有時 `open()` 回報失敗，其他輸出不受影響。

## Core Audio（macOS）

| 模式 | 做法 |
|---|---|
| 獨佔 | hog mode（只有這個程序能在裝置上播放，也沒有其他聲音混進來）；裝置取樣率設成要求值（裝置有提供時；否則同家族、不超過要求的最高取樣率，再否則裝置的最高取樣率，`matches_request()` 為假）。播放器要求轉換（`convert`，最後一次 PCM 嘗試）而裝置的取樣率不是要求值的整數倍時（只有 48 kHz 的 DAC 收 44.1 kHz），輸出回報要求的取樣率，由 `AudioConverter` 轉成裝置的取樣率（`system_rate()`，回呼數以串流的 frame 計算）；沒有要求時從不轉換，DoP 因此一定原樣送到 DAC。保留的裝置留著上一首的轉換器時，開啟時先丟掉。DAC 實體格式調成該取樣率下位元數最多的整數格式（至少 24 bit，已足夠就不動，不重新鎖定；DAC 沒有 24 bit 以上的整數格式時的行為尚未確認，N26） |
| 共享 | 系統混音器：裝置維持自己的取樣率，不改實體格式、不拿 hog。裝置取樣率是要求值的整數倍時，輸出回報裝置取樣率，由播放器的鏈升頻（44.1 k → 88.2 kHz）；不是整數倍時（48 k 在 44.1 kHz 的裝置上、96 k 在 44.1 kHz 上），輸出回報要求的取樣率，在 IO 回呼裡用 Core Audio 的 `AudioConverter`（最高品質）轉成裝置取樣率，與 WASAPI 共享的 `AUTOCONVERTPCM` 相同（播放器的鏈只會整數倍升頻） |

- 樣本格式：環裡是 24-bit 整數靠左放在 32 bit（`int24-in-32`），IO 回呼寫進串流的 virtual format（32-bit 浮點，目前的 macOS 驅動都是）時除以 2^31。32-bit 浮點能精確表示每個 24-bit 整數，獨佔模式下 HAL 把浮點轉成 DAC 的整數格式也是精確的，所以 24 bit 以內的 PCM 與 DoP 都一個位元不差地送到 DAC（與 MIKU 自己的 Core Audio 輸出、Audirvana、Roon 在 macOS 上的作法相同）。播放器看到的裝置格式就是 `int24-in-32`：DoP 的標記照常寫在最高位元組（`dop_capable`），bit-perfect 的判斷也照 24 bit 計算。
- hog mode 的裝置只在 IO 運轉中改設定：停止的 hog 裝置改了取樣率之後再放掉 hog，這個程序之後就無法在它上面開始 IO（`AudioDeviceStart` 卡住數秒，MIKU 的 Core Audio 輸出遇過）。所以 `open()` 先拿 hog、建立 IO procedure 並**先啟動 IO**（送靜音），再改取樣率與實體格式；`close()` 在 IO 還在跑時先還原實體格式與取樣率、等 IO 在原取樣率又跑了兩次回呼，才停止 IO、放掉 hog。
- 取樣率變更：設定後每 10 ms 讀回，最多 1.5 s；裝置沒有接受時以它實際的取樣率繼續（`matches_request()` 為假，播放器會重新設計或重試），並記錄一則訊息。格式變更後等 HAL 回報的 virtual format 都是新的取樣率、且連續兩次讀到相同（最多約 1 s）才開始。
- 「系統輸出」（`device` 空白或 `default`）：裝置被 hog 時 macOS 會把系統輸出移到別的裝置（只有一個裝置時就沒有系統輸出），放掉 hog 之後過一會兒才移回來。所以依序是：本程序 hog 住的裝置；本程序以系統輸出身分拿到、放掉不到 3 秒的那一台（播放器重啟裝置時先關再開，中間的空窗期裡系統輸出不存在，或暫時是另一台裝置：之前偶爾因此找不到裝置而退回原取樣率，有兩台裝置時還會開到另一台）；系統輸出（不存在時最多等 1 秒）。系統輸出不存在而有裝置被其他程序 hog 時，回報「… is in exclusive use by another application (pid N)」，而不是「沒有系統輸出」。
- Windows 的輸出工廠收到 `coreaudio_*` 時回報「Core Audio is macOS only」（原本會當成 WASAPI 獨占開啟）。
- 保留裝置（`keep_output_devices`，`devices.h`；C API `dc_keep_device`，helper 預設打開）：與 MIKU 自己的 Core Audio 輸出相同，獨佔模式的 `close()` 不還原，裝置保留著（hog、取樣率、DAC 格式，IO 繼續跑、送靜音；IO 回呼這時沒有 owner，只寫零），下一次 `open()` 同一台裝置時直接接手，新的取樣率在運轉中直接設定（DAC 只重新鎖定一次，同取樣率則完全不動），macOS 也不會在中間把系統輸出移走再移回。要求共享模式、另一台裝置、或保留的裝置已被拔除時，先把它還原再開。`release_kept_output_device()` 還原（取樣率回到第一次開啟前的值、DAC 格式）並放掉 hog。程序結束時不呼叫 HAL：helper 在 `quit` 時放開；程序掛掉時系統放掉 hog，但取樣率停在最後的值（與 MIKU 相同）。
- 被其他程序 hog 住的裝置：獨佔與共享都開不了（不會卡住）。獨佔、或以系統輸出開啟而系統輸出不存在時，回報「… is in exclusive use by another application (pid N)」；共享模式指定裝置 UID 時不檢查 hog 的持有者，要到啟動 IO 才失敗，訊息是無法啟動。
- 聲道：立體聲用裝置偏好的立體聲聲道（`kAudioDevicePropertyPreferredChannelsForStereo`）；單聲道寫到那兩個聲道；多聲道從第 1 個聲道依序對應；裝置聲道不夠時（5.1 在立體聲 DAC 上）以立體聲開啟，由處理鏈降混（與 ASIO 相同）。每次回呼先把整個 buffer list 清成 0，再寫入用到的聲道。
- IO 緩衝：`output.buffer_ms` 換算成 HAL 的 buffer frame size，限制在裝置的範圍內；0 用裝置預設（內建輸出 512 frames）。
- 通知：裝置被拔除（`DeviceIsAlive`）→ `failed()`（「the output device was removed」）；其他程式改了取樣率 → `reset_requested()`（播放器在原位置重開）。
- IO 回呼不上鎖、不配置記憶體；`stop()` 等正在讀環的回呼離開後才返回（之後播放器可以釋放環）。
- 延遲：裝置延遲 + safety offset + 串流延遲 + IO 緩衝。

## 裝置列表（`devices.h`）

- `list_wasapi_devices(probe)`：作用中的 render 端點（ID、名稱、預設、混音器格式）；probe 時以 `IsFormatSupported` 測試獨占模式下 44.1k–768k × 16/24/32-bit 的立體聲格式。
- `list_asio_drivers()`：`HKLM\SOFTWARE\ASIO` 下安裝的驅動（不需要 SDK）。
- `list_coreaudio_devices()`（macOS）：有輸出聲道的裝置（UID、名稱、是否為系統輸出、目前取樣率、聲道數、提供的標準取樣率、連接方式、hog 的持有程序）。`output.device` 用 UID（重開機後不變），也接受清單索引與名稱。
- CLI：`daphne devices [--probe]`（macOS 列出 Core Audio 裝置）；C API：`dc_devices` 的 `coreaudio` 陣列。

## 驗證

| 項目 | 結果 |
|---|---|
| `daphne devices`：列出 6 個 WASAPI 端點與 2 個 ASIO 驅動 | 正確 |
| WASAPI 獨占，44.1 kHz、24 / 16-bit 隨機樣本（含滿刻度極值）錄回比對（`verify/live.py` 第 1 節） | 每個樣本都相同（bit-perfect） |
| WASAPI 獨占 176.4 kHz：無縫跨取樣率、ReplayGain 換軌、DoP 傳輸（第 2–4 節） | 全部通過，underrun 0 |
| ASIO | 編譯通過；ASIO 只能透過實體裝置測試（會發出聲音），尚待實機聆聽確認 |
| Core Audio（macOS 11.7、Intel、內建輸出：44.1 / 48 / 88.2 / 96 kHz、24-bit 整數） | `daphne_tests "[.coreaudio]"`（隱藏測試，CI 不跑）3 個全過：獨佔把 44.1 kHz 換成 96 kHz、hog 由本程序持有、實體格式 24 bit，播完 `drained()`，關閉後取樣率回到 44.1 kHz、hog 放開；共享維持 44.1 kHz、不拿 hog；播放器以 96 kHz `int24-in-32` 播到結尾，之後裝置還原 |
| Core Audio，CLI | 44.1 k → 96 k 連續兩首：都是 bit-perfect、underrun 0，第二首換取樣率，結束後回到 44.1 kHz；44.1 k 以 `2x` 升到 88.2 kHz、`buffer_ms` 50（延遲 52 ms）：underrun 0；兩個程序：第一個獨佔播放中，第二個（獨佔或共享）立即回報「Built-in Output is in exclusive use by another application (pid N)」，第一個照常播完 |
| Core Audio，播放中重啟裝置（2x 升頻打開：44.1 k → 88.2 k） | 修正前約 5 次 2 次留在 44.1 kHz（見上「系統輸出」）；修正後 8 次都換到 88.2 kHz；隱藏測試「關閉後立刻以另一個取樣率重開」通過 |
| Core Audio，外接 USB DAC（44.1–384 kHz，實體格式只有 32-bit 整數，DSD 最高 DSD128） | 隱藏測試 `[.coreaudio]` 4 個全過；CLI 獨佔連播 44.1 / 96 / 192 / 352.8 / 384 kHz：每首換取樣率、都是 bit-perfect（播放器的判斷）、underrun 0、延遲約 4–15 ms，結束後回到 44.1 kHz；`auto` 升頻 44.1 k → 352.8 kHz（4096 taps）underrun 0。換取樣率的那一首「最長事件間隔」約 135 ms（開裝置時 DAC 重新鎖定的靜音期間，第一首約 14 ms） |
| Core Audio，DoP（同一台 DAC） | dsd 核心 DSD64（176.4 kHz）、DSD128（352.8 kHz）：DAC 面板依序顯示「176 → D64 → 44.1」、「352 → D128 → 44.1」（開頭先送無標記的靜音，DAC 先當成 PCM），underrun 0；Intel i5 兩核心處理速度 DSD64 5.9 倍、DSD128 3.9 倍即時 |
| Core Audio，外殼（同一台 DAC） | `verify/miku_shell.js` 獨佔、共享、`verify/helper.py` 全過。獨佔時 macOS 把系統輸出移到內建輸出，放掉後約 1–3 秒才移回；播放（含 2x 升頻重啟到 88.2 kHz）都在 DAC 上，內建輸出的取樣率沒變過。停止後立刻 probe「系統輸出」會讀到內建輸出（只影響顯示） |
| Core Audio，共享的取樣率轉換（同一台 DAC，44.1 kHz） | 修正前 48 k 檔在共享模式直接失敗（「the device locked 44100 Hz, not an integer multiple of the source rate 48000 Hz」）；修正後 96 k / 192 k / 48 k / 44.1 k 連播都播到結尾、underrun 0，前三首記錄「Core Audio converts … to the device's 44.1 kHz」；共享模式回報「bit-perfect: no (shared mode (system mixer))」。隱藏測試加上「共享轉換非整數倍的取樣率」，5 個全過 |
| Core Audio，其他程式改取樣率（同一台 DAC） | 共享：播放中由另一個程序改成 88.2 kHz → 「the device rate was changed to 88.2 kHz by another application: reopening」，在原位置以 88.2 kHz 重開（鏈升頻），播到結尾、underrun 0、最長事件間隔 6 ms；獨佔：另一個程序的變更被 macOS 拒絕（`'!hog'`，kAudioDevicePermissionsError），裝置維持 44.1 kHz，bit-perfect 播完 |
| Core Audio，保留裝置（同一台 DAC，helper） | 44.1 k → 96 k → 44.1 k 三首：hog 全程同一個程序，取樣率直接切換（每次一次重新鎖定，log「the device was kept from the last track」），`stop` 帶 `hold` 保留、不帶就還原到 44.1 kHz 並放掉 hog。96 k 接 96 k：換曲 30 ms、取樣率不動；不保留時（CLI）是 96 k → 44.1 k → 放掉 hog → 再拿 → 96 k，約 1.7 s。隱藏測試「a kept device is taken over …」、`miku_shell.js`（獨佔：stop 後 `holdsDevice` 與 `kept` 為真，`release()` 後沒有裝置被 hog）、`helper.py` 全過 |
| Core Audio，loopback 錄音（BlackHole 2ch，獨佔） | 隱藏測試 `[.blackhole]`：PCM 44.1 / 96 / 192 / 384 kHz 的 int24 雜訊錄回來逐個樣本相同；DoP DSD64 / DSD128 錄回來 0x05 / 0xFA 標記完整（2026-10-16 全過）。BlackHole 內部存 float32，所以這只驗證 Daphne 送出的樣本沒被改動，不涵蓋真 DAC 上 float → 整數的轉換 |
| Core Audio，播放中拔除 DAC（MIKU，獨佔） | MIKU 顯示「the output device was removed」並停止，沒有卡住（2026-10-16） |
