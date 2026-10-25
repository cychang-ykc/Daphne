# N26 待辦

尚未完成的工作與已知、暫不處理的問題。完成的項目移到對應的文件（N25 等），並從這裡刪除。

## 尚未驗證

- **MSVC ARM64 建置**（Windows on ARM）：程式已避開 x86 專屬的 cpuid、FPCR 改用 `_ReadStatusReg` / `_WriteStatusReg`，但沒有 ARM64 工具組，尚未實際編譯。
- **Linux 即時排程**：處理執行緒拿到 `SCHED_FIFO` 時，由它建立的執行緒（濾波器設計、`parallel_for`、無縫換軌預備）會先回到 `SCHED_OTHER`。需要有即時權限（`RLIMIT_RTPRIO` / `CAP_SYS_NICE`）的 Linux 實際確認。
- **macOS universal 建置的 arm64 部分**：在 Intel Mac 上建置成功（x86_64 有 AVX2 核心、arm64 沒有），但 arm64 只由 CI（macOS 14 原生）執行過，沒有執行過 universal 二進位檔的 arm64 部分。
- **較舊的 FFmpeg**：pkg-config 建置驗證過 FFmpeg 6.1（Ubuntu 24.04，CI）、8.0、8.1；Debian 12 套件庫的 FFmpeg 5.1 未測試。
- **Windows 上的小項目**：
  - 本分支與 `main` 的效能比較（`parallel_for` 的 worker 優先權在 Windows 改成三級，原本照抄建立者的確切值），用最重的設定比較 underrun 數與設計時間。
  - `decode-test --md5`（內建 MD5，取代 BCrypt）與 `main` 的結果逐檔比對。
  - 含中文、emoji、超過 260 字元的實際檔名播放（孤立 surrogate 已有單元測試）。
- **8 階 DSD64 的最佳化 NTF 在實機上**（N19）：處理鏈中量測與單元測試都通過。
  - [x] DoP 實機播放（2026-10-22，Audiobyte USB Driver 1.0.62，WASAPI 獨佔，`0f96912`）：48 kHz / 96 kHz 24-bit FLAC 各 20 s，131072 taps、double-double，176.4 kHz DoP → DSD64；underrun 0、event timeout 0、clipped 0，聽感正常。
  - [ ] 原生 DSD（ASIO）播放。
  - [ ] 類比量測確認 40 – 100 kHz 的超音波噪聲。

## 拆分計畫的後續步驟（N25）

順序：0 → 5 → 7，以 Windows 與 macOS 為主；6（Linux）暫緩。每一步一個分支、一個 PR，直接合進 `main`。完成的項目打勾，實作時的發現記在最後的「實作筆記」。

### 0. 輸出能力（分支 `output-caps`，PR #18）✅

- [x] `player` 依輸出 API 判斷的三件事（有沒有原生 DSD、是不是共享混音、緩衝長度由誰設定）改成由各平台的輸出後端提供（`OutputCaps`），開裝置前就查得到；刪掉 `common.h` 的 `api_has_native_dsd` / `api_is_shared` / `api_takes_buffer_ms`，播放器不再認得 API 名稱。
- [x] Windows 的 `WorkerPool` worker：替處理執行緒工作時用 `RealtimeHelperScope`（MMCSS "Audio"），不再只是 `THREAD_PRIORITY_ABOVE_NORMAL`。
- [x] 驗證：Windows 全部測試、`live.py`（行為不變）；三平台 CI。（PR #18：Windows 93 個測試、`live.py` 全過、CI 四個 job 通過）

### 5. 即時輸入的安全卸離（分支 `live-bus`，PR #19）✅

範圍在 2026-10-16 縮小：MIKU 1.4.5（`a15a6f6`）起，Mac 上的 YouTube Music 一律由網頁經系統輸出播放，播放核心只需要讓出裝置（見步驟 7）；把網頁的聲音擷取下來、經 `live-relay.js` 交給核心的路線預設關閉（設定檔要寫 `"ytThroughCore": true`，原因是在真實網頁上啟動不穩定）。所以先做 A（網頁自己播），B（經過 Daphne）只保留接口。

- [x] 安全卸離：解碼器只在 `produce()` 裡碰共用的環，期間持有租約（`g_readers`）；`set_live_bus` 先清掉指標、等租約歸零，再換上新的環。返回之後 host 就能釋放記憶體，播放中也可以（Windows MIKU 的共用記憶體也受益）。
- [x] 測試：一條執行緒不停讀取，另一條反覆卸離 / 直接替換環，並在返回後立刻把舊的環填成 NaN 再釋放；輸出不得出現 NaN 或其他值。
- [x] B 的接口（未實作）：`live_connect` / `live_disconnect`（`live_source.h`）與 C API 的 `dc_live_socket`，目前一律回傳失敗；`dc_live_bus` 先結束 socket 來源再換環。

**B 之後要補做時（Unix socket 來源）**：路徑在 MIKU 給模組的 `host.liveSocket`；資料是交錯的 float32 立體聲、沒有標頭、可能在任何位元組切開（一個 frame 會跨兩次 `recv`）；`live-relay.js` 會重啟（MIKU 0.5 s 後再開），要自動重連；讀得慢的連線會被丟區塊（積壓超過 1 MB）；取樣率由 MIKU 呼叫 engine 的 `setLiveRate(rate)` 告知（網頁要求 48 kHz，`tap` 訊息回報實際值）；Mac 外殼要回報 `supportsLive = true`，MIKU 才會走這條路。

### 7. macOS Core Audio 輸出與 MIKU 的 Mac 外殼

拆成四個 PR：7a 輸出本身、7b 排程、7c helper 程序、7d MIKU 的 Mac 外殼。

**7a. Core Audio 輸出（分支 `coreaudio-output`，PR #20）✅**

- [x] Core Audio 輸出（共享 / 獨佔），照 MIKU 的 `mac/native/audio.mm`（N24「Core Audio」）：
  - 設定 nominal rate 後輪詢確認，不接受就以裝置實際的取樣率繼續（播放器重新取樣）並留下記錄；
  - 獨佔時才把 DAC 實體格式調到該取樣率下位元數最多的整數格式（至少 24 bit），已足夠就不動（不重新鎖定）；
  - 格式變更後等 HAL 回報的格式連續兩次相同才開始；
  - hog 住的裝置只在運轉中改設定：`open()` 先啟動 IO（靜音）再改取樣率與格式，`close()` 在 IO 運轉中還原，最後才停止 IO、放掉 hog；
  - 裝置拔除（`failed()`）、外部改取樣率（`reset_requested()`）的通知。
- [x] `output.api` 加上 `coreaudio_exclusive` / `coreaudio_shared`，預設是平台的獨佔輸出（`kDefaultOutputApi`）；CLI 的 `--shared` / `--exclusive` 跟著平台。
- [x] DSD 只走 DoP（macOS 沒有原生 DSD 的 API）：裝置格式是 `int24-in-32`，DoP 照常寫入，回呼精確轉成 float32。（外接 DAC 上 DSD64 / DSD128 確認，DAC 顯示 D64 / D128）
- [x] 裝置列舉：`list_coreaudio_devices()`、`daphne devices`、C API `dc_devices` 的 `coreaudio` 陣列。
- [x] 實機驗證（Intel Mac、macOS 11.7、內建輸出）：隱藏測試 `[.coreaudio]` 3 個、CLI 跨取樣率連播、升頻、兩個程序搶裝置（N24「驗證」）。
- [x] 外接 DAC 驗證（USB DAC，44.1–384 kHz、DSD128）：隱藏測試 4 個、CLI 44.1–384 kHz 連播 bit-perfect、`auto` 升頻到 352.8 kHz、DoP DSD64 / DSD128（DAC 顯示 D64 / D128）、`miku_shell.js` 獨佔 / 共享、`helper.py`，全部通過（N24「驗證」）。
- [x] 其他程式改取樣率：共享時在原位置以新的取樣率重開；獨佔時 macOS 拒絕其他程序的變更（`'!hog'`），照常播完（N24「驗證」）。
- [x] 共享模式的取樣率轉換（分支 `coreaudio-shared-src`）：裝置取樣率不是要求值的整數倍時（48 k 在 44.1 kHz 的裝置上），原本直接失敗；改成輸出在 IO 回呼裡用 `AudioConverter` 轉換，與 WASAPI 共享的 `AUTOCONVERTPCM` 相同。順便修正共享模式回報 bit-perfect 的問題（Windows 的 WASAPI 共享也一樣）：改回報「shared mode (system mixer)」。
- [x] bit-perfect 錄音比對：BlackHole 2ch 當 loopback，隱藏測試 `daphne_tests "[.blackhole]"`（PCM 44.1–384 kHz int24 逐樣本相同、DoP DSD64 / DSD128 標記完整，2026-10-16 全過）。錄音要麥克風權限：沒有權限只會錄到全零、不報錯；使用者在桌面上給「終端機」權限後，從 ssh 執行的同一個測試程式也錄得到。BlackHole 存 float32，不涵蓋真 DAC 上 float → 整數的轉換。
- [x] DSD 在 DoP 鎖不到時轉成 PCM，配合 DAC 鎖定的取樣率（分支 `dsd-pcm-at-locked-rate`）：2026-10-16 USB DAC 拔掉再接回去後，MIKU 播 DSD64 時 DAC 要 176.4 kHz 卻鎖在 88.2 kHz，轉 PCM 又固定用 176.4 kHz，鏈只能往上升頻，所以「not an integer multiple」整首播不出來（連續 6 次）；之後再拔插兩次都沒重現，DAC 一接回來就回報完整的 44.1–384 kHz。改成：獨佔模式下 DoP 被鎖在較低的取樣率時，若它低於平常的 176.4 / 192 kHz 而且能整除 DSD 取樣率，就用它轉 PCM（共享模式本來就配合混音器）。同時把 DoP 嘗試時輸出端的說明（取樣率改不過去等）寫進 log，下次再發生可以看到原因。這也讓最高只到 96 kHz 的 DAC 能在獨佔模式播 DSD 檔（原本一定失敗）。
- [x] 播放中拔除 DAC（2026-10-16，使用者在 MIKU 裡以 Daphne 獨佔播放時拔掉 USB DAC）：MIKU 的進度再走幾秒，接著顯示 Daphne 的錯誤「the output device was removed」，沒有卡住。Daphne 在 HAL 通知 `kAudioDevicePropertyDeviceIsAlive` 變成 0 時立刻標記失敗，播放器每一圈（幾 ms）都檢查，所以那幾秒應該是 macOS 從拔除到發出通知的時間（從程式判斷，沒有量）。
- [ ] 外殼 `probeDevice(null)` 在停止後的頭 1–3 秒會讀到內建輸出（獨佔時 macOS 把系統輸出移走，放掉後才移回）：可改成沿用最後一次播放的裝置，或在外殼裡也套用 core 的「剛放掉的系統輸出」規則。只影響顯示：core 開裝置時已套用該規則，停止後馬上播下一首仍送到 DAC。只有 macOS 有這個現象（Windows 的 WASAPI 獨佔不會更改預設輸出，ASIO 與預設裝置無關）。暫緩：等 MIKU 介面內的整合測試（7d）時，真的看到顯示閃一下再處理。`verify/miku_shell.js` 偶爾在停止後讀到 88.2 kHz 也是同一件事（查過，不是 bug）：外殼的 `stop()` 送出就不等回覆，0.5 s 後的 probe 讀到的是還沒關完的裝置。helper 的 `stop` 平常 0.84 s（USB DAC 在 IO 運轉中改回取樣率約 0.83 s，內建輸出約 0.33 s）。如果剛送了要換取樣率的 `apply` 就馬上 stop，stop 要 1.7 s：從時間看，是 stop 等那次重開先把排隊的聲音放完、淡出（`restart()` 的 drain，約 0.7 s），再加上還原取樣率的 0.84 s。最後取樣率都正確還原、hog 也放掉（每 10 ms 監看確認）。
- [x] 只播放音樂，不播濾波器延遲（分支 `fix-track-end-tail`）：濾波器把整段訊號延後一個延遲（linear phase 約 taps 的一半：8192 taps 約 0.09 s，1048576 taps、intermediate 約 8 s）。原本開頭先播一段延遲（無聲），結尾只 `flush()` 剩下的輸入，最後一首（或不接續的下一首）少了一個延遲長度的音樂；1M taps 播 6 s 的檔，整首音樂都沒播出來（CLI「last position 0 s」）。所有平台都有。改成開頭丟掉延遲（開始、跳轉、重開），結尾送零到最後一個輸入樣本出來為止（它之後的餘振不播），輸出長度等於原曲 × 倍率。
- [x] 開裝置時中間失敗的嘗試寫進 log（同一分支）：PCM 的取樣率逐級下降、DSD 的原生 → DoP → 較低倍率，非最後一次失敗時送事件「the output at … failed (原因): trying …」，原本只有最後一次的錯誤。
- [x] 換曲時保留裝置（分支 `coreaudio-keep-device`，只做 macOS）：原本每次重開裝置都先還原再設定取樣率（跨取樣率的下一首多一次 DAC 重新鎖定），每換一首也放掉再拿 hog（系統輸出跟著移走移回）。改成與 MIKU 相同：獨佔模式下裝置保留著（hog、取樣率、DAC 格式），下一首同一台裝置直接接手、在運轉中改取樣率；何時放開由 MIKU 決定（`stop({ release: true })` / `release()`：離開 MIKU 且沒在播、播 YouTube、結束），不加計時器。96 k 接 96 k 從約 1.7 s 變成 30 ms。Windows 不做：WASAPI 獨佔與 ASIO 關閉時不還原任何設定，也不搬系統輸出，換曲只有重開那一下。
- [ ] 跳轉後的開頭（觀察，之後）：跳轉時鏈先 `reset()`，所以跳轉點之後約半個濾波器長度的輸出只用到跳轉點之後的樣本（濾波器前面的歷史是零）。原本就是如此，只是以前前面多了一段無聲延遲（`fix-track-end-tail` 之後開頭直接是音樂）。如果之後在大 taps 下跳轉聽到喀聲，可以在跳轉後加一個短的淡入（與重開裝置的 20 ms 淡入相同）。

**7b. 排程（不改程式，量測結果記在實作筆記）✅**

- [x] 重新檢查 macOS 的 `RealtimeScope`：目前是 time-constraint policy（每 10 ms 最多 5 ms），但處理執行緒是一次補滿約 0.5 s 環的吞吐型工作，超過宣告的計算時間可能被系統降級。候選：處理執行緒改用 QoS user-interactive，真正的即時工作只留在 IOProc（環 → 裝置，已是 HAL 的即時執行緒）；`WorkerPool` 的 worker（步驟 0 起跟著拿 time-constraint）一起決定。在 Mac 上加 CPU 負載量 underrun 決定。→ 結論：維持 time-constraint（滿載下比 QoS user-interactive 多拿到約 35% 的處理速度，兩者都沒有 underrun）。

**7c. helper 程序（分支 `mac-helper`，PR #21）✅**

- [x] 一個執行檔，stdin / stdout 每行一個 JSON（與 MIKU 的 `miku-audio` 相同的模式），由 JS 外殼啟動（`helper/main.cpp`，`daphne-helper`，協定見 N4）。掛掉時重啟並接回原本的位置是 JS 外殼的事（7d）。
- [x] 讓出與取回裝置（A 方案必需）：Mac MIKU 播 YouTube Music、或 MIKU 不在前景且沒有播放時，會檢查 engine 的 `holdsDevice`，呼叫 `stop({ release: true })` 或 `releaseDevice()`；Daphne 要完全放開裝置（還原取樣率與格式、放掉 hog，7a 的 `close()` 已經做到），下一首曲庫的歌再取回。對照 MIKU 的 `native-audio.js`。→ helper 的 `stop` 就是讓出（`verify/helper.py` 在 Mac 上確認 stop 之後沒有裝置被 hog、取樣率還原），下一次 `load` 取回；`holdsDevice` 由 JS 外殼依載入 / 暫停狀態回答（7d）。

**7d. MIKU 的 Mac 外殼（分支 `mac-shell`，PR #22）✅**

- [x] `miku/extension.js`（MIKU 從 `Extensions/<名稱>/miku/` 載入，提供 `core.createEngine`），外殼回報 `supportsLive = false`。不需要改 MIKU。移植自 Windows 擴充（`DaphneExtension.cs`、`DaphneEngine.cs`、`DaphneOptions.cs`、`DaphnePath.cs`），底下是 helper；頁面檔案共用（`miku/web` symlink）。helper 掛掉時重新啟動並從原位置繼續（一分鐘最多 3 次）。
- [x] 驗證：Intel Mac（macOS 11）上用模擬 MIKU 的腳本測 helper、讓出 / 取回裝置。（`verify/miku_shell.js`，獨佔與共享都全部通過）
- [x] MIKU 介面內的整合測試：MIKU 的 Mac 版用 Electron 44，需要 macOS 12 以上，介面內的整合測試要等有新的 Mac（或試舊版 Electron 的開發模式）。→ 改用 Electron 37（支援 macOS 11 的最後一版；38 起需要 macOS 12）以開發模式在測試機上跑，只為測試，不改 MIKU：`~/daphne-test/miku`（MIKU 的 `mac/app`、`Extensions/Daphne` 連到 `~/daphne-test/Daphne`、FFmpeg 6.0、原生程式 `audio.mm` 以 macOS 11 SDK 編譯，只要把 `kAudioObjectPropertyElementMain` 換成同值的 0；它的 `--self-test` 在 macOS 11 全過），雙擊 `Run MIKU (Electron 37).command` 啟動。從 ssh 啟動時主程式載入沒有錯誤，但沒有人登入桌面時不會開視窗；等有人登入 Mac 的桌面再做介面內的測試。 → 2026-10-16 使用者登入桌面後在 MIKU 1.4.5（Electron 37）裡實際操作，全部正常：Daphne 擴充載入；44.1 k 升頻到 352.8 k，換 taps 時在背景建好再切換；DSD（DoP）播放；換曲時保留裝置（log「the device was kept from the last track」）；MIKU 不在前景且沒在播時放掉裝置。另外看到 MIKU 掃描音樂庫時 ffprobe 的時間上限是 20 秒（`ffmpeg.js` 的 `run()`），NAS 上 420 MB 的 DSF 要讀 29.7 秒，所以記錄「ffprobe failed」（之後 MIKU 自己讀 DSF 檔頭補資料）；這是 MIKU 的行為，與 Daphne 無關，不改。
- [x] MIKU Mac 安裝包帶入 Daphne（分支 `miku-mac-pack`）：Windows 與 macOS 用同一份 `Extensions/Daphne`。Windows 由 MIKU 掃描 `Extensions/*/src/*/*.Extension.csproj` 編譯；macOS 的 MIKU 打包腳本原本寫死另一個資料夾，改成同樣的規則：掃描 `Extensions/*/miku/pack-mac.sh`，由模組自己打包（MIKU 那邊的修改在 MIKU 的分支上）。`miku/pack-mac.sh <out>` 輸出 `<out>/<arm64|x64>/daphne/`：`miku/*.js`、`web/`（symlink 展開成檔案）、`LICENSE`、`bin/` 裡的 `daphne-helper`、`libdaphne_core.dylib`（與 Windows 的 `daphne_core.dll` 同一份核心）和 FFmpeg 的 dylib。Intel Mac 上兩種架構共 502 s（FFmpeg 只在第一次編）。

### 6. Linux ALSA 輸出（暫緩）

- [ ] ALSA 輸出：`hw:`（獨佔、bit-perfect、DoP）與 `default`（共享，經 PipeWire / Pulse）；`libasound` 在建置時可有可無。
- [ ] double-double 的 `WorkerPool` worker 優先權：步驟 0 起 worker 替處理執行緒工作時已經進入 `RealtimeHelperScope`（Linux 為 `SCHED_FIFO`，見下方實作筆記）；還要在有即時權限的 Linux 上確認兩核心的機器不會被 FIFO 的 worker 佔滿（處理鏈比即時慢時）。

### 實作筆記

（依完成順序記錄）

**步驟 0（`output-caps`）**

- 能力要在開裝置**之前**知道（`pcm_stream` / `dsd_stream` 依它排出要嘗試的取樣率，`core.cpp` 依它決定 DSD 檔轉 PCM 的取樣率），所以不放在 `AudioOutput` 物件上，而是與 `OutputOpener` 並列的 `OutputCapsQuery`（`PlayerDeps::output_caps`，以 `OutputChoice` 查詢）。`change_plan` 的 `Playing` 帶著目前輸出的能力。
- 預設值 `OutputCaps{}`（沒有原生 DSD、不是共享、吃 `buffer_ms`）就是 WASAPI 獨占，也是測試假裝置原本的行為，所以既有的測試在三個平台都不變；Linux 的 `platform_output_caps()` 回傳預設值；macOS 的在 7a 加上 Core Audio 共享為 `shared_mix`（`output_mac.cpp`）。新增測試：假裝置宣告 `native_dsd` 時 DSF 直接以 DSD 開啟、宣告 `shared_mix` 時 dsd 核心拒絕。
- 錯誤訊息裡仍寫著 ASIO / WASAPI（「output.dsd = native needs an ASIO driver with a DSD mode」、dsd 核心的「needs WASAPI exclusive or ASIO」），在 Windows 上正確；原本預定加入 Core Audio 時改成不提 API 名稱的寫法，7a 之後還沒改，macOS 的使用者會看到 Windows 的 API 名稱（`pcm_stream.cpp`、`dsd_stream.cpp`）。
- worker 優先權沒有在建立時決定：每個重新取樣器有自己的 `WorkerPool`，背景預熱（N4）時替普通執行緒工作，換上之後替處理執行緒工作。改成 `run()` 記下呼叫者是否在 `RealtimeScope` 內（新的 `platform::in_realtime_scope()`，thread_local），worker 只在這個狀態改變時進入 / 離開 `RealtimeHelperScope`。這是三個平台共用的程式：Linux 的 worker 因此也會跟著拿 `SCHED_FIFO`（沒有權限時維持原狀），兩核心機器的實測仍待步驟 6；macOS 的 worker 拿 time-constraint，與 `RealtimeScope` 一起在步驟 7 重新檢查。

## 已知問題（暫不處理）

- **Vorbis 的搜尋偶爾不精確**：Linux 上 20 秒的 Vorbis 檔隨機 seek 300 次後讀 4096 frames，與整檔解碼比對有 3 次不同（main 與預讀相同，其他格式 0 次）。原因未查（FFmpeg 解碼器提前 4096 frames 起解，見 N2）。
- **處理執行緒的 sleep 輪詢**（環滿時 2 ms、排空時 5 ms、等待裝置的迴圈）：之後改成事件等待。
- **helper 在 Mac 上實機確認**：✅ 2026-10-22，`e7db5e6`，Intel Mac（macOS 11.7）、Core Audio 獨佔、Audiobyte BLACK DRAGON（USB），以模擬 MIKU 的腳本驅動 helper（同 N4「helper 程序」的情境：1M taps 第一次設計中排第二首，再按停止，PCM 176.4 kHz）。連跑 4 次：排隊的那首 0 ms、設計中的那首 2 – 76 ms 回 `DC_ERR_CANCELLED`，停止 0.82 – 0.89 s，之後再載入正常播放；合併 #45 前的 helper（`adaa2b6`）在同一台 Mac（BlackHole）上停止要約 160 s，回覆時狀態已是 playing。DAC 在設計前就被 hog（要先設好取樣率才知道濾波器怎麼設計），但 `start()` 之前 IO 回呼只送靜音，取消後不會開始播；停止的 0.8 s 是把取樣率改回原本的 352.8 kHz 並等 DAC 確認（與一般停止相同）。沒有在 Mac 的 MIKU 介面裡操作（使用者決定不需要）。
- **Windows 擴充在 MIKU 裡實機確認**：✅ 2026-10-22，本機 publish 的 MIKU 1.4.5（獨立的設定資料夾、WASAPI 獨佔、VB-Audio Hi-Fi Cable、1M taps、double-double、16 個設計執行緒、濾波器快取關閉），經 WebView2 遠端除錯呼叫頁面自己的 `Host.call`（與按鈕相同），每 50 ms 以 `SendMessageTimeout` 量 UI 執行緒。設計中停止：2 ms 返回，沒有出聲、沒有錯誤。設計中換歌（下一首、下一首、上一首，各隔 3 s）：被跳過的立刻取消，最後一首約 52 s（一次設計）後播放，沒有錯誤；修正前四首依序各設計一次，150 s 內沒有一首開始播。設計中關閉：視窗 0.11 – 0.13 s 消失、程序 0.2 s 內結束，與沒在播放時相同；修正前約 5 s（設計裡沒有檢查取消的段落，N4）。三種情境都沒有 UI 停頓（只有啟動時一次）。
- **8 階 DSD64 的迴路重置**（N19）：沒有頻寬限制的過載直接送進調變器時，新的表比原本的表容易重置迴路（約 1.5 – 2 倍）：停在限幅器天花板的 40 Hz 方波 3 秒重置 410 次（原本 2 – 3 次）；4 倍滿刻度方波在 440 Hz、1 kHz（44.1 kHz 輸入）重置 526 – 1599 次（原本 0 次），50 / 100 Hz 兩者都會重置（原本約 1200 – 1300 次，新表約 2100 次）。處理鏈的升頻濾波器與限幅器讓這種輸入不會出現，處理鏈中的測試都是 0 次；若之後有不經升頻濾波器與限幅器的輸入路徑，要重新檢查。

### 2026-10-25 review（PR #17 – #52）發現的問題

每一項標有下列其中一種依據：

- **重現**：在 Linux 上以單元測試或實際執行看到問題。
- **讀程式**：讀程式或編譯結果確認，沒有執行。
- **待 Mac 實機**：讀程式推論，只有在真的 Mac 上測試才能確認。

每一項修正另開分支，並附回歸測試。測試草稿保留在 review 的工作區。

#### A. 會讓程序結束或播出錯誤聲音的問題


#### B. 取消載入（#45 – #51）


**R10. Windows 擴充：較新的載入提早返回時，舊的載入沒人管**（讀程式）

- 問題：舊的那首在濾波器設計完後開始播，但不設 `_loaded`。暫停、跳轉、電平都失效，只有停止能結束它。
- 原因：`LoadAsync` 先遞增 `_loadSeq`。即時輸入的頁面還沒好（`!LiveBus.Ready`）時，`LoadAsync` 在 `dc_cancel` 之前就返回。舊的那首看到序號變了，就不設 `_loaded`。

**R11. Windows 擴充：`ReconfigureAsync` 可能蓋掉使用者剛選的歌**（讀程式。只有 MIKU 在換歌時同時呼叫 `ReconfigureAsync` 才會發生）

- 問題：`ReconfigureAsync` 在 `_gate` 外記下目前的歌，停止後重新載入這首。這段期間使用者選的新歌拿到較舊的序號，於是被舊歌的重新載入取代。
- #50 之前，兩者依閘門的順序執行，新歌最後播。

#### C. MIKU 的 Mac 外殼與打包

**R13. 連續兩次快速改 DSD 播放方式時，外殼忽略第二次**（讀程式）

- 問題：檔案以第一次的設定播放，`loadedDsd` 卻記成第二次的設定。之後的比較都錯，直到下一首。
- 原因：`engine.js` 的 `load()` 在 `await` 之後才以當時的設定算 `loadedDsd`。第一次的重新載入還沒完成就改第二次時，`applyOutput()` 看到 `loaded == false`，只送 `changed`。
- 修正：以外殼實際送出的設定算 key。載入完成後再比對一次 key。

**R14. 打包沒有帶齊授權檔**（讀程式）

- 問題：Windows 給 MIKU 的打包（`MikuDeploy`）只複製 DLL 與 `web/`，沒有 Daphne 的 GPL `LICENSE`、FFmpeg 的 LGPL 授權與 FFmpeg 的原始碼位置。兩邊的打包都沒有 nlohmann/json 的 MIT 授權。核心與 helper 都編進了這個函式庫，MIT 要求附上授權文字。
- 修正：複製 `LICENSE` 與 `third_party/nlohmann/LICENSE.MIT`。Windows 另外複製 FFmpeg 的授權與 `SOURCE.txt`。

**R15. FFmpeg 版本改了之後，打包腳本沿用舊的 FFmpeg 編譯結果**（讀程式）

- 問題：`bin/` 裡是舊版的 FFmpeg，授權與原始碼位置卻寫新版，所以 LGPL 的原始碼位置是錯的。
- 原因：`pack-mac.sh` 只檢查 `build-mac-<arch>/ffmpeg/lib/libavcodec.dylib` 是否存在，不比對 `FF_VER` 或 configure 選項。Windows 的 `build_for_miku.ps1` 也只在 `avcodec.h` 不存在時才下載 FFmpeg。
- 修正：以版本與選項寫一個 stamp 檔。

**R16. `pack-mac.sh` 的另外三個問題**（讀程式）

1. checkout 路徑含空白時，`for rp in $(otool …)` 會把 rpath 切開，`install_name_tool` 失敗，腳本中止。FFmpeg 的 configure 本來也不接受含空白的路徑，但至少會明確地報錯。
2. 最後一行啟動 helper，確認它能執行。在 `set -o pipefail` 下，helper 失敗時腳本直接結束，看不到 helper 自己的錯誤訊息。
3. `codesign … 2>/dev/null` 藏起了簽章失敗的原因。

#### D. 即時輸入的小問題（讀程式）

**R17. `set_live_bus` 沒有鎖，也不檢查 `cap_frames`**

- 兩條執行緒同時換環時，可能留下環 A 的指標配環 B 的容量。
- 播放中傳入負的容量會變成 `0xFFFFFFFF`，解碼器接著讀到環外。
- `open()` 會拒絕 ≤ 0 的容量，但對已開啟的解碼器呼叫 `set_live_bus` 時不會拒絕。
- 修正：加鎖。`base` 不是 NULL 時，拒絕 `cap_frames <= 0`。

**R18. 2³² 回繞時，「被追過」的判斷用錯環的大小**

- 回繞的那一刻，環只剩 2³² % cap 個 frames，但「被追過」的判斷仍以整個容量計算。
- 積壓可能超過那一段。以 MIKU 的 96000 來說，界線約是 0.49 s。
- 這時解碼器讀到宿主已經覆寫的資料，而且不重新同步。約 27 小時發生一次，出現一個雜音（N2）。

**R19. 即時輸入的統計與 `dc_live_socket` 的回傳值不清楚**

- 卸離期間，`dc_state` 的 `live.fill` 等統計停在最後的值。
- `dc_live_socket` 在 `path` 為 NULL 時與尚未實作時都回傳 0，宿主分不出原因。

#### E. 待 Mac 實機（讀程式推論）

**R20. 引擎放開保留的裝置時，取樣率可能不對**

- `raise_physical` 第一次調高實體格式時，記下舊的格式，而這個格式含當時的取樣率。引擎每次開啟都重算 `rate_touched`，值不累積。
- 例：96 kHz 的那首調高了格式，接著 44.1 kHz 的那首接手裝置（`rate_touched` 為假）。放開時還原舊格式，會把 DAC 鎖回 96 kHz。
- 目前驗證用的 DAC 只有 32-bit 整數格式，`raise_physical` 從沒改過它。

**R21. `raise_physical` 可能降低位元數**

- 目前的實體格式是浮點、或是另一個取樣率時，`cur_bits` 為 0，`raise_physical` 會選上任何整數格式，包括 16 bit。
- 沒有 24 bit 以上的整數格式時，開啟也不失敗。輸出仍回報 `int24-in-32`（DoP 可用、bit-perfect），但 DAC 收到的是 16 bit 的資料。

**R22. 保留裝置的 `close()` 有與 R4 相同的交握問題**

- `close()` 清掉 `owner`（release）之後，以 `acquire` 讀 `in_render`；`render()` 先 `fetch_add`，再讀 `owner`。這種 Dekker 式交握需要 `seq_cst`。
- x86 上 store 可以排到 load 之後，所以 IO 回呼可能在物件釋放後寫入 `stats_`。

**R23. 共享模式轉換器的小問題**（獨佔時要求轉換的也一樣）

- `AudioConverterFillComplexBuffer` 在 IO 執行緒上執行。N24 說 IO 回呼不配置記憶體，但這個函式不保證。
- 轉換器還留著幾個 frame 時，`drained()` 就成立，曲子最後幾 ms 沒有播出。
- 跳轉時沒有重設轉換器。
- `latency_ms` 不含轉換器的延遲。
- `resolve()` 的 `std::stoul` 遇到很長、全是數字的裝置字串時會丟例外。

#### F. 沒有測試涵蓋的部分

- 打包腳本不在 CI：`pack-mac.sh`、`build_for_miku.ps1`、`fetch_*.ps1`，以及 C# 擴充的建置。
- `miku/*.js` 沒有任何檢查，連 `node --check` 都沒有。
- macOS 的 CI 沒有以 FFmpeg 建置（`DAPHNE_FFMPEG_DIR`）。
- 取消：
  - 快取打開時取消濾波器設計，快取裡不能留下檔案。
  - `parallel_for` 的例外與取消。
  - 載入進行中呼叫 `dc_destroy`。
  - helper 的佇列與世代號。
  - C# 擴充的取代邏輯。
- 輸出能力：
  - `takes_buffer_ms = false` 時，改 `buffer_ms` 不能重開裝置。
  - `native_dsd` 與 dsd 核心，以及 `output.dsd = dop`。
- TSan job 只跑 `[player],[capi],[live]`，不跑調變器的 worker 執行緒（`[modulator]`）。

#### 用語

| 用語 | 本節的意思 |
|---|---|
| 宿主 | 使用引擎的程式：經 Mac 外殼的 MIKU，或 Windows 擴充。 |
| Mac 外殼 | `miku/extension.js` 與 `engine.js`，在 macOS 上替 MIKU 啟動 helper。 |
| helper | `daphne-helper`，從 stdin 讀 JSON，底下用 C API。 |
| Windows 擴充 | MIKU 在 Windows 上載入的 C# 擴充（`src/Daphne.Extension`）。 |
| 環 | 宿主把即時音訊交給解碼器的共用記憶體環（live bus）。 |
| 租約計數 | `g_readers`，計算目前正在使用環的解碼器讀取。 |
| 卸離 | 把環從解碼器拿掉（`set_live_bus` 傳 NULL 或新的環）。 |
| 保留的裝置 | 引擎在兩首之間持有的 Core Audio 裝置（`dc_keep_device`）。 |
| 共享模式 | 經系統混音器輸出（WASAPI 共享或 Core Audio 共享）。 |
| `pm` 區段 | C API 裡持有 mutex `pm` 的程式段落。 |

## MIKU 擴充（`src/Daphne.Extension`，暫緩）

- **裝置失效（0x88890004，AUDCLNT_E_DEVICE_INVALIDATED）**：設定中的裝置不在（例如 DAC 關機）時，MIKU 自己的核心會退回預設裝置，Daphne 只回報錯誤。改成退回預設裝置，或顯示看得懂的訊息。
- **播放路徑卡片**：
  - 加上 ReplayGain 的狀態（目前是否套用、增益多少）。
  - DSD 直送時，DSP 區加一列說明（直送不經任何處理）。
- **設定頁**：不升頻時也顯示「濾波器暫存」那一列（目前只在升頻或轉 DSD 時出現）。

**步驟 5（`live-bus`）**

- 租約只包住 `produce()`（真正碰環的地方），不包住 `read()` 等待來源的 2 ms sleep：卸離最多等一次內插的時間。讀取端 `fetch_add` 之後才讀指標、卸離端清掉指標之後才讀計數；這種交握要兩邊的讀取都是循序一致（seq_cst），「讀取端拿到舊指標」與「卸離端看到計數為 0」才不會同時成立。卸離端的計數讀取原本是 acquire，不夠：Apple clang 以 `-mcpu=apple-m1` 編成 `stlr`（清指標）接 `ldapr`（讀計數），ARMv8.3 允許 `ldapr` 排到 `stlr` 之前；一般 ARM64 是 `ldar`，x86 的 seq_cst store 是 `xchg`，都沒有這個問題。2026-10-25 改成 seq_cst，編譯結果變成 `ldar`（以 zig 0.16 內含的 clang 編 `aarch64-macos` 確認）。這種重排在單元測試裡抓不到。
- 換環時先清掉指標、等計數歸零，再設定新容量、最後放上新指標：讀取端不會拿到新指標配舊容量（原本 `g_cap` 與 `g_base` 分開寫，換環的一瞬間可能錯配）。
- 壓力測試要確認真的抓得到錯誤：暫時拿掉等待那一行後，第一版（寫入方每 1 ms 推進 2400 frame）6 次只失敗 1～2 次，因為讀取端很快讀完，大部分時間在 `read()` 的 sleep 裡（Windows 上一次約 15.6 ms），卸離很少撞上讀取。改成每 10 µs 推進 48 frame 的細流之後，讀取端幾乎一直在環裡，拿掉等待時 6 次全部失敗（其中兩次直接存取已釋放的記憶體而當掉）；恢復等待後 10 次全部通過。
- 測試的等待用 `steady_clock` 忙等，不用 `sleep_for(1 ms)`：Windows 沒有提高計時器解析度時那是 15.6 ms，第一版測試因此跑了 25 秒（現在約 1.6 秒）。

**步驟 7a（`coreaudio-output`）**

- DoP 與 float32：Daphne 的 DoP 只接受 24 / 32-bit 整數的裝置格式，而 Core Audio 的 IO 回呼收的是 32-bit 浮點。輸出對播放器宣稱 `int24-in-32`，回呼再除以 2^31 轉成浮點：24-bit 整數在 float32 裡精確，HAL 再轉成 DAC 的整數格式也精確，PCM 與 DoP 都不必另外處理。
- 「系統輸出」在 hog 之後會消失：第一次兩個程序搶裝置的測試裡，第二個程序拿到的是「no system output device」——裝置被 hog 時 macOS 把系統輸出移走，內建輸出是唯一的裝置，系統輸出就變成沒有。改成：本程序 hog 住的裝置仍算系統輸出（同一個程序重開時也找得到），系統輸出不存在而有裝置被別人 hog 時回報是誰佔著。
- Apple clang 12 的 `-Wunused-private-field`：`RealtimeScope::handle_` 只有 Windows 用，在這台 Mac 以 `DAPHNE_WARNINGS_AS_ERRORS` 建置時才報錯（CI 的 Xcode 15 不報，之前在 Mac 上沒有開 `-Werror`）；加上 `[[maybe_unused]]`。
- 隱藏測試（`[.coreaudio]`）只在實機上手動跑：CI 的 macOS runner 不保證有音訊裝置。測試訊號是 −60 dBFS，夜裡在內建喇叭上跑也幾乎聽不到。
- 跨取樣率的下一首比較慢（1.5 s 的 96 kHz 檔播了 2.4 s）：關閉時還原 44.1 kHz、開啟時再設 96 kHz，DAC 重新鎖定兩次。列為之後的改進（保留裝置）。

**步驟 7b（排程的量測）**

- Intel Mac（i5-4308U，2 核 4 執行緒）、Core Audio 獨佔、48 k → 96 k、1048576 taps、double-double（無負載時 2.5 倍即時），另外開 4 個 `yes` 把 CPU 吃滿，各跑兩次：time-constraint（現在的作法）2.55 / 2.74 倍即時，QoS user-interactive 1.85 / 2.02 倍；underrun 都是 0，最長回呼間隔都在 16–26 ms。time-constraint 沒有因為超過宣告的計算時間而變慢，反而在滿載下拿到更多 CPU，所以維持不變（`WorkerPool` 的 worker 一樣）。實驗用環境變數切換，只在 Mac 上編譯，沒有進 repo。
- 順帶看到：1048576 taps 時 CLI 結尾的「last position」是 0 s（262144 taps 時 3.8 s）。→ 不只是顯示：最後一首的結尾少了一個濾波器延遲長度的音樂，已修正（見「只播放音樂，不播濾波器延遲」）。

**步驟 7c（`mac-helper`）**

- helper 底下直接用 C API（與 Windows 擴充相同），沒有另寫一套播放邏輯；JSON 用 nlohmann（已在 `third_party`）。
- `quit` 一開始在工作執行緒裡 `fclose(stdin)` 想打斷讀取執行緒的 `getline`，Windows 上打斷不了阻塞中的 pipe 讀取（`verify/helper.py` 的 quit 項目逾時）；改成由讀取執行緒自己處理 `quit`。
- 主程式結束時 stdin 讀到結尾，走與 `quit` 相同的清理：MIKU 當掉也不會把 DAC 留在 hog mode。

**步驟 7d（`mac-shell`）**

- 外殼介面以 MIKU 的 `AudioEngine`（`mac/app/main/audio.js`）為準：它把模組的 engine 當成 backend，用到 `load` / `pause` / `resume` / `seek` / `stop` / `release`、`holdsDevice`、`isPlaying` / `isLoaded` / `position` / `track` / `signal` / `st`、`applyVolume` / `applyDsp` / `applyReplayGain` / `applyOutput`、`listDevices` / `probeDevice`、事件 `changed` / `loading` / `failed` / `trackStarted` / `ended`。Mac 設定頁看核心的 `info` 旗標：`autoSampleRate: false`、`dsdModes`、`maxDsd`、`dsdPcmRate: false`；套用 Bit-perfect 時呼叫 `ext.daphne.bitPerfect`（關掉 Daphne 的升頻、apodize、double-double）。
- Mac 與 Windows 設定的差異：輸出模式是 `coreaudio` / `coreaudio-exclusive`；沒有 DAC 硬體音量（`hardware` 當成數位音量，與 MIKU 自己的核心相同）；DSD 只有 DoP（獨佔）或 PCM；EQ 多了 `LPQ` / `HPQ` 兩個類型名稱。
- `verify/miku_shell.js` 第一次跑有 5 項失敗，前 4 項是測試本身：替身的 `peekNext` 一直回傳 B（B 播完又排 B，無限接續），1 秒的檔案在設定變更前就播完了。改成 B 之後沒有下一首、後半段用 8 秒的檔案。
- 第 5 項是真的問題（2x 升頻打開後裝置留在 44.1 kHz，約 5 次 2 次）：播放器重啟裝置時先關舊的、再開新的；舊的放掉 hog 之後 macOS 還沒把系統輸出移回來，新的找不到「系統輸出」（「no system output device」），播放器的 88.2 kHz 嘗試失敗，退回清單裡的 44.1 kHz，而且失敗的嘗試訊息被丟掉。修正見 N24「系統輸出」（記住剛放掉的那一台 3 秒、最多等 1 秒）。播放器把非最後一次的嘗試失敗也記錄下來會比較好查，列為之後的改進。
- 同一個空窗期也讓 `probeDevice` 在 stop 之後一瞬間找不到系統輸出：改成退回剛才播放的那一台，再退回清單第一台。
- stop 之後 0.5 秒查詢時偶爾看到 88.2 kHz（最後的 `bitPerfect` 重啟與 stop 幾乎同時）；2 秒後三次都回到 44.1 kHz，裝置最後都還原。
- 頁面檔案：MIKU 的載入器只讀模組資料夾下的 `web/`，而且只允許那個資料夾裡的檔案；`miku/web` 是 git 的 symlink（mode 120000），macOS / Linux checkout 時是真的 symlink，Windows（`core.symlinks=false`）是一個內容為路徑的文字檔，Windows 擴充不用它。MIKU 的 Mac 安裝包裡展開成實際的檔案（`miku/pack-mac.sh` 用 `cp -RL`）。
- Mac 上用免安裝的 Node 20（`~/daphne-test/tools`）跑測試；MIKU 本身（Electron 44）需要 macOS 12，介面內的整合測試仍待新的 Mac。

**DAC 驗證後的修正（`coreaudio-shared-src`）**

- 共享模式失敗的原因：WASAPI 共享在取樣率不同時請 Windows 轉換（`AUTOCONVERTPCM`），播放器從來不會遇到非整數倍；Core Audio 的 IOProc 直接寫裝置，必須是裝置的取樣率，而播放器的鏈只會整數倍升頻（`chain.cpp`「not an integer multiple」）。之前的共享測試都用 44.1 k 的檔案，所以沒發現。
- 只有非整數倍才交給 `AudioConverter`：整數倍仍由 Daphne 自己的升頻濾波器做（品質較好，這是 Mac 與 Windows 不同的地方：WASAPI 共享只要取樣率不同就交給 Windows）。轉換器在 IO 回呼裡向環要資料（`conv_input`，一次最多 `tmp_frames`），建立與配置都在 `open()`，回呼裡不配置記憶體。
- DSD 檔在共享模式轉 PCM 時，`core.cpp` 用輸出回報的取樣率決定 DSD → PCM 的取樣率。轉換器的條件是「裝置取樣率低於要求值，或不是它的整數倍」，而探測時要求的是 DoP 取樣率（DSD64 為 176.4 kHz），所以 44.1 與 48 kHz 的裝置都回報 176.4 k：DSD 先轉成 176.4 k，再由 `AudioConverter` 降成裝置取樣率（48 kHz 的裝置之前會失敗）。原本寫的「44.1 kHz 的裝置仍是 44.1 k」與程式不符（2026-10-25 review）；改由 Daphne 直接轉成 44.1 k 需要另一種方式取得裝置本身的取樣率。
- bit-perfect 的判斷原本不看共享模式（`Report::bit_perfect`），Windows 的 WASAPI 共享也回報 yes；MIKU 的頁面自己判斷品質（共享一律是 high），所以畫面沒錯，只有 CLI 與 report 錯。Windows 擴充的 `DaphnePath.cs` 用 `bit_perfect` 判斷「64-bit 浮點轉成裝置的浮點格式，數值不變」，原因是共享模式時也視為數值不變（交給混音器的樣本本身沒變）。
- 其他程式改取樣率的測試工具：在 Mac 上編一個小程式，用 `kAudioDevicePropertyNominalSampleRate` 改取樣率（與「音訊 MIDI 設定」相同的方式），播放中執行。

**只播放音樂，不播濾波器延遲（`fix-track-end-tail`）**

- 重採樣器輸入 N 個樣本只輸出 N × 倍率個（`test_resampler.cpp`），輸出第 j 個是輸入時間 (j − (L − 1) / 2) / 倍率；`flush()` 只算完還沒滿一個區塊的輸入。gapless 接續（`join_produce`）本來就先送 2 倍延遲的零（上一首的餘振與下一首疊在一起，維持不變），只有串流真正結束時漏了。
- 測試：結尾前 10 個樣本放一個脈衝，2x、8192 taps，linear 與 minimum phase 修正前都沒有出現在輸出（峰值 1e-11 / 1e-14），位置停在 40004 / 44079（應為 44100）。
- 第一版照 gapless 送 2 倍延遲的零：1M taps 的 6 s 檔播了約 22 s（開頭約 8 s 延遲、結尾約 8 s 餘振）。改成只播音樂，與 r8brain 的作法相同（處理器自己吃掉起始延遲）：開頭丟掉鏈輸出的前 `content_delay` 個 frame（`skip_out`，`abs_out` 照算，位置、gapless 與 crossfade 的對齊都不變），結尾送 ⌈延遲 / 倍率⌉ 個零。Mac 上 1M taps 的 6 s 檔輸出 576001 frames（96 kHz 的 6.00 s）。
- 測試：中間的脈衝剛好在輸出 44100（22050 × 2），結尾的在 88180，總長不超過 88200 加裝置緩衝；linear 與 minimum phase。
- 跳轉時鏈先 `reset()`，所以跳轉點之後約半個濾波器長度的輸出只用到跳轉點之後的樣本（原本也是如此，只是前面多一段延遲）。
- 補零開始後不再接 gapless 的下一首（`tail_zeros` ≥ 0），以免同一段尾巴被送兩次。

**換曲時保留裝置（`coreaudio-keep-device`）**

- 保留的裝置是一個 `CoreAudioOutput::Impl`（一次一台），放在全域；它的 IO 回呼從關閉的那一刻起沒有 owner（`owner` 改成 atomic，回呼先數 `in_render` 再讀 owner，關閉時清掉 owner 後等 `in_render` 歸零），只寫零。`open()` 接手時沿用 hog、IO procedure 與第一次開啟前的取樣率（`original_rate`，最後放開時還原成它），重新設定聲道配置、實體格式與緩衝；`last_ns` 歸零，否則換取樣率的空檔會被算成一次 late callback。
- 開檔失敗時（例如串流格式不支援）直接還原，不保留。
- 打開與否是整個程序的設定（C API `dc_keep_device`）：helper 打開，CLI 與 Windows 擴充不打開，行為不變。
- 播放器送出 `ended` 之後還要等裝置緩衝播完（約 30–50 ms）才關閉輸出，所以那一刻問 `kept` 還是假；外殼在獨佔模式收到 `ended` 就當作保留著（`failed` 是在輸出關閉之後才送，問 helper 即可）。
- 量測（同一台 DAC，`ratewatch` 每 10 ms 看 hog 與取樣率）：hog 從第一首到 `release()` 都是同一個程序；44.1 k → 96 k → 44.1 k 每次一次重新鎖定；96 k 接 96 k 換曲 30 ms。CLI（不保留）連播 96 k 兩首：還原 44.1 kHz、放掉 hog、再拿、設 96 kHz，約 1.7 s。

**MIKU Mac 安裝包（`miku/pack-mac.sh`）**

- helper 與核心分開（`daphne-helper` 連 `libdaphne_core.dylib`），和 Windows 一樣是「同一份核心函式庫 + 平台的外殼」：Windows 的 C# 擴充用 `DllImport` 在 MIKU 的程序裡載入 `daphne_core.dll`；Electron 的主程序不能直接載入原生函式庫，所以 Mac 由 JS 外殼啟動 helper，helper 再載入 dylib。helper 的 rpath 是 `@loader_path`（`helper/CMakeLists.txt`），打包時兩者放在同一個 `bin/`。
- FFmpeg：與 Windows 同版本（8.1.3，LGPL、動態連結），從原始碼編（ffmpeg.org 的 tarball，SHA-256 寫在腳本裡；ffmpeg.org 沒有公布 SHA-256，這個值是寫腳本時下載的檔案的），只開引擎需要的 demuxer / decoder（沒有專屬解碼器的格式：M4A / ALAC / AAC、Ogg、Opus、APE、WavPack、WMA、TTA…）。`--install-name-dir=@rpath`，所以核心連到的是 `@rpath/libavcodec.62.dylib` 等，打包時照這些名字複製。沒有 FFmpeg 時這些格式在 Mac 上不能播（測試機的音樂庫有 46 首 ALAC）。
- CMake：macOS / Linux 的 FFmpeg 原本只用 pkg-config 找（測試機的 ssh 環境找不到 pkg-config），改成也可以指定資料夾（`DAPHNE_FFMPEG_DIR`，含 `include/` 和 `lib/`）；Windows 的路徑不變。
- 每個 arch 分開編（`build-mac-arm64/`、`build-mac-x86_64/`，最低 macOS 11.0），因為 MIKU 的安裝包也是每個 arch 一份。打包後刪掉所有絕對路徑的 rpath，檢查 `otool -L` 只剩 `@rpath/` 和系統的函式庫，ad-hoc 簽章（`install_name_tool` 會讓簽章失效；MIKU 的安裝包會再簽一次），在本機 arch 上啟動 helper 確認 hello。
- `bin/` 只放程式（MIKU 的安裝包會簽 `bin/` 裡的每個檔案）；FFmpeg 的授權（`FFmpeg-COPYING.LGPLv2.1`）與原始碼位置（`FFmpeg-SOURCE.txt`）和 `LICENSE` 一起放在模組根目錄。`.gitattributes` 讓 `*.sh` 在每個 checkout 都是 LF：Windows（`core.autocrlf`）checkout 的 CRLF 腳本複製到 Mac 會在 `set -o pipefail` 失敗（`pipefail\r`）。
- 結果（Intel Mac）：每個 arch 約 4 MB；arm64 版全是 arm64、x64 版全是 x86_64；x64 的 helper 回報 `{"abi":3,"e":"hello","version":"daphne 0.1.0"}`。arm64 版要在 Apple Silicon 的 Mac 上試。
