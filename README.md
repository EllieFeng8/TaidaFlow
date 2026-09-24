# TaidaFlow

TaidaFlow 是 Qt Design Studio 產生的 Qt Quick HMI(主畫面 / 警報 / 歷史)。本分支 `core`
含真實後端 `Core/`:`core.cpp`、`manager.cpp`、`Modbus_Client`(5 台 ADAM,預設
`192.168.1.201~205:502`,會寫 DO/AO;**測試用**環境變數 `TAIDAFLOW_DEVICE_PROFILE=simulator`
改連本機模擬器 `127.0.0.201~205:502`,見「接 Adam60xxSimulator」)、`Modbus_Server`(bind `AnyIPv4:502`)、
`Ms300FaultReader`(Modbus RTU `COM2`)、`RESTManager`、`SqlManager`(SQLite)、
`HistoryExport`(歷史 CSV 匯出佇列 + 下載服務 `0.0.0.0:8124`,見「歷史資料:時間區間與匯出」)。

QML 只面對 `Core/TaidaFlowProxy.h`(QML singleton `Td`,module URI `TaidaFlowBackend`)。
透過 `integration-pack/wasm-mirror`(pack **1.0.1**,wire protocol 3,**唯讀、由維護方提供**,
整包複製,`MANIFEST.sha256` 24/24),同一份程式可編成:

- **Desktop(Windows MSVC)**:authoritative 端。`Core::instance().init()` 建立並擁有
  `TaidaFlowProxy`、啟動全部硬體後端,並開 `ws://127.0.0.1:8125/mirror` Mirror server。
- **WebAssembly**:replica 端。**不編譯任何後端**(Desktop-only Core,pack 文件
  package-integration §1/§5.4、guide §10/§11),`main.cpp` 直接建立 `TaidaFlowProxy` 當 replica,
  瀏覽器頁面連回 desktop,雙向同步 `Td` 的全部 Q_PROPERTY。

## 安全注意(開 desktop 前必讀)

desktop 版會對 `192.168.1.201~205:502` 建 Modbus TCP 連線並**寫入** DO/AO(泵浦、閥、變頻器、
緊急停止迴路),開 `COM2`,並在 `0.0.0.0:502` 開 Modbus server。在非現場的開發機上:

- 一律用 `scripts\run-desktop.ps1` 啟動。它先跑 `scripts\safety_probe.ps1`(對 5 台 ADAM 只做
  TCP connect、1.5 秒逾時、**不送任何 Modbus**;列出本機序列埠;檢查 502/8123/8124/8125 是否已有人
  listen),任一裝置可達、出現 `COM2` 或 502 已被占用 → **不啟動**(exit 3);8125(mirror)或
  8124(CSV 下載服務)已被占用 → 不啟動(exit 4,不會關掉別人的程式)。每次探測都附加
  寫入 `docs/evidence/wasm-v4/safety-probe.log`。
- desktop 另在 `0.0.0.0:8124` 開 CSV 下載服務(內網、不做存取控管;只提供匯出資料夾裡的檔,見下方)。
- 工作目錄固定為 `build\runtime-cwd\`:Core 會在「目前工作目錄」寫 `TaidaFlowSettings.ini`、
  `settings.sqlite`、`data\sensor_YYYYMM.sqlite`(及 REST 用的 `device_info.ini`),網頁匯出的 CSV
  寫在 `exports\`。`build/` 已被 `.gitignore` 排除,work tree 保持乾淨。
- 不要為了接模擬器改網路設定(不加 IP alias)或改 Core 程式;要接模擬器請用下方
  「接 Adam60xxSimulator」的測試用 profile。`run-desktop.ps1` 在預設模式會**移除**
  `TAIDAFLOW_DEVICE_PROFILE`,確保 app 用的就是探測過的 192.168.1.x。

## 需求環境

| 項目 | 路徑 / 版本 |
|---|---|
| Qt | 6.8.3:`C:\Qt\6.8.3\msvc2022_64`、`C:\Qt\6.8.3\wasm_singlethread` |
| CMake / Ninja | `C:\Qt\Tools\CMake_64\bin\cmake.exe`、`C:\Qt\Tools\Ninja\ninja.exe` |
| MSVC | VS 18 Community `vcvars64.bat` |
| Emscripten | emsdk **3.1.56**(Qt 6.8 對應版)於 `C:\tools\emsdk` |
| Python 3 | 開發腳本用;字型子集需 `fonttools`,截圖比對需 `Pillow` |

所有建置輸出都在 `build/` 下(已被 `.gitignore` 排除)。
注意:本分支 `.gitignore` 排除了 `/CMakePresets.json`,提交時需 `git add -f CMakePresets.json`。

## 建置

`CMakePresets.json`:`desktop-release`(MSVC,`build\desktop`)、`wasm-debug` / `wasm-release`
(Qt `wasm_singlethread` toolchain + emsdk 3.1.56 chainload,`QT_HOST_PATH` 指向 msvc2022_64,
build preset 繼承 configure 環境)。

```bat
scripts\build-desktop.bat [fresh]                 :: vcvars64 + cmake --preset/--build desktop-release
scripts\build-wasm.bat [wasm-release|wasm-debug] [fresh]
```

WASM 產物:`build\wasm-release\TaidaFlowApp.html / .js / .wasm`、`qtloader.js`
(Release `.wasm` 約 33.6 MB;本次 33,641,584 bytes)。

### CMake 結構(Desktop-only Core)

- 根 `CMakeLists.txt`:先判斷 `EMSCRIPTEN` 再判斷 `WIN32`(`TAIDAFLOW_IS_WASM` /
  `TAIDAFLOW_IS_WINDOWS_DESKTOP`)。共用 components:`Core Gui Widgets Qml Quick QuickTimeline
  ShaderTools Network WebSockets`;**只有 Desktop** `find_package` `SerialBus SerialPort Sql
  HttpServer Concurrent`。QDS 預設的 `CMAKE_INCLUDE_CURRENT_DIR ON` 保留原狀(pack 1.0.1
  自行在子目錄關閉,宿主端不加 workaround)。
- `Core/CMakeLists.txt`:QML module `Core` 的共用來源只有 `TaidaFlowProxy.h`;後端 `.cpp/.h`
  與上述 Qt 模組的 link 只在 Desktop 分支加入。`Core` link `WasmMirror::Core`,並在此 target
  `wasm_mirror_register_proxy(CLASS TaidaFlowProxy)` + `finalize`(兩端共用同一 header/contract)。
- `App/CMakeLists.txt`:`SerialBus`/`Sql` link 只在 Desktop(`main.cpp` 只在 Desktop include `core.h`)。

驗證 WASM 真的沒有後端:`python scripts\check_wasm_backend.py build\desktop build\wasm-release`
(看 `ninja -t commands`、最終 link 的函式庫、CMakeCache 與二進位字串)。

## 執行(desktop + 瀏覽器)

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -Label "manual"   # 先探測再啟動
python scripts\serve_wasm.py            # 綁 0.0.0.0:8123;本機開 http://127.0.0.1:8123/TaidaFlowApp.html
python scripts\serve_wasm.py --host 127.0.0.1   # 只給本機(舊行為)
```

- `run-desktop.ps1` 設 `QT_FORCE_STDERR_LOGGING=1`、PATH 加 Qt bin,log 寫到
  `build\runtime-logs\`。啟動 log 應含 `[ModbusServer] listening on 0.0.0.0:502 unit=1`、
  `WASM Mirror endpoint: ws://127.0.0.1:8125/mirror` 與
  `[ExportHTTP] download service listening on 0.0.0.0:8124`(8124 被占用時只記 warning,app 照常執行)。
- `serve_wasm.py`:預設綁 **`0.0.0.0`**(內網其他電腦可開網頁;`--host 127.0.0.1` 改回只給本機);
  送 `application/wasm`、`Cache-Control: no-store`、COOP/COEP(符合
  `integration-pack/wasm-mirror/docs/http-server-requirements.md`)。
  注意:本分支 desktop mirror 仍是 `127.0.0.1:8125`、`allowedOrigins` 只有 `127.0.0.1:8123` /
  `localhost:8123`;區網電腦要連 mirror 需 main 的區網連線工作調整 mirror 綁定與 allowedOrigins。
- 瀏覽器 console:`WASM Mirror ready: true` 即同步完成。

### 離線提示(transport overlay,pack §9)

`TaidaFlowProxy` 有兩個 **`STORED false`**(不進 Mirror contract)的本機屬性 `transportReady`
(member 預設 `true`)與 `transportMessage`。只有 WASM 的 `main.cpp` 在載入 QML 前設成
`false / "Connecting to the Qt desktop Core..."` 並安裝 `transportStateHandler`。
`transportReady == false` 時:

- `TopNav.qml` 顯示紅色「離線」橫幅(含 transport 訊息);
- 所有會寫同步 property 的控制項 disabled:變頻器復歸、緊急停止、M1~M4 數值對話框、
  泵浦開關對話框、泵浦頻率對話框、二通閥對話框(已開的對話框會被關閉)、歷史頁上/下一頁
  (`historyCurrentPage` 是同步屬性);
- 畫面保留最後一份 authoritative 狀態;重連並收到完整 snapshot 後自動恢復。

desktop 上 `transportReady` 永遠是 `true`,外觀與行為不變(見驗證 4)。
純本機的檢視控制(分頁切換、警報頁分頁)不受影響;歷史頁的日期篩選 / 顯示全部 / 下載 CSV / 取消
改由 desktop Core 執行(request signal 經 mirror),離線時由 HistoryPage.qml 停用。

## 接 Adam60xxSimulator(測試用設備位址切換)

`Adam60xxSimulator`(repo 內另一個專案,**只執行它已建好的 exe**,不建置、不在其資料夾寫檔)
以 `--autostart` 在 `127.0.0.201~205:502`(Unit ID 1)開五台 ADAM。Core 的切換只在
`Core/Modbus_Client.cpp`:

| `TAIDAFLOW_DEVICE_PROFILE` | 五台 ADAM 位址 | log |
|---|---|---|
| 未設定 / 空字串 | `192.168.1.201~205:502`(與原本相同) | `[Modbus] device profile=default` + 五行位址 |
| `simulator` | `127.0.0.201~205:502`(port、unit 不變) | `[Modbus] device profile=simulator` + 五行位址 |
| 其他值 | 維持 `192.168.1.x` | 先記 `[Modbus] Unknown TAIDAFLOW_DEVICE_PROFILE="…"` 警告 |

只換位址:重連、補送、interlock、MS300(COM2)、聚合 Modbus server(`0.0.0.0:502`)行為都不變。
這是**開發測試用**切換;正式的設備位址可設定化仍屬維護方建議(`docs/wasm-integration-report.md` §5 建議 8)。

```powershell
# 1. 先起模擬器(工作目錄 build\sim-cwd;502 已被占用就拒絕,不會關掉別人的程式)
powershell -ExecutionPolicy Bypass -File scripts\run-simulator.ps1
# 2. 再起桌面版(simulator 模式探測 + 幫 app 設 TAIDAFLOW_DEVICE_PROFILE=simulator)
powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -DeviceProfile simulator -Label "sim"
# 3. 網頁版照舊
python scripts\serve_wasm.py            # http://127.0.0.1:8123/TaidaFlowApp.html
```

- **順序一定是模擬器先**。實測(`docs/evidence/wasm-v4-sim/14-bind-order-core-first.txt`):
  app 先起時,它對 `127.0.0.20x:502` 的 ADAM 連線會被 app **自己的** `0.0.0.0:502` 聚合 server
  接走(讀值全 0、6224/6256 回 Modbus exception),之後再起模擬器也不會換過去。所以
  `safety_probe.ps1 -DeviceProfile simulator` 要求五個模擬器端點都已在聽,否則 exit 5
  (`SIMULATOR-NOT-READY`),`run-desktop.ps1` 不啟動。模擬器先起時,app 的 `0.0.0.0:502`
  仍 bind 成功,兩者並存(連 `127.0.0.20x` 的連線由較精確的模擬器 bind 接手)。
- simulator 模式的探測:照舊 TCP 探測 `192.168.1.201~205:502`、列序列埠(可達或有 COM2 → exit 3);
  port 502 的 listener **只**接受「程序映像檔是 `Adam60xxSimulator.exe` 且位址在
  `127.0.0.201~205`」,其他(別的程式、別的位址、IPv6)一律 UNSAFE(exit 3)。紀錄預設寫到
  `docs/evidence/wasm-v4-sim/safety-probe.log`。預設模式(不帶 `-DeviceProfile`)行為與輸出不變。
- `-DeviceProfile simulator` 不能與 `-PvFile`(dev-only PV 注入)並用。
- 已知點位落差(不是切換造成,之後另案處理):模擬器 ADAM-6224 DI0 是「漏液」且預設 0,
  Core 把 DI0 當「相位正常」→ 啟動後立即 `[Safety Interlock] ... DI0 is false`,泵浦啟動會被擋;
  Core 補水泵寫 6256 coil 19,模擬器製程只把 coil 16 當泵浦。
- 模擬器只在值**改變**時記 `Core → ADAM-…` log(`QModbusServer::dataWritten`);寫入同值不會出現,
  以 Core log 的 `[Modbus][Write completed]` 為準。
- D2 自我測試(真的起模擬器與假 listener,exit code 判定):
  `powershell -ExecutionPolicy Bypass -File scripts\probe_selftest_sim.ps1`。
- 注意:`run-desktop.ps1` / `run-simulator.ps1` 的輸出若接到管線(`| Select-Object` 等),被啟動
  的程式可能繼承該管線,指令要等程式結束才返回;直接執行或導向檔案即可。

## 中文字型(WebAssembly)

Qt for WebAssembly 沒有系統 CJK 字型,因此內嵌 **Noto Sans TC 子集**(OFL-1.1):

- 產生:`python scripts\make_font_subset.py`。掃描 `App/ Core/ TaidaFlow/ TaidaFlowContent/
  Dependencies/` 的 QML/JS/C++ 非 ASCII 字元(涵蓋 core 分支後端全部中文)+ 可列印 ASCII +
  常用全形標點,實體化 wght 400/700 並子集化:`App/fonts/TaidaFlowNotoSansTC-{Regular,Bold}.ttf`
  (184,856 / 185,544 bytes,490 字元,CJK 385)、`App/fonts/charset.txt`。
  **新增中文字串後要重跑**;`--check` 在字元集變動或缺字時 exit 1。
- 來源字型:`C:\Windows\Fonts\NotoSansTC-VF.ttf`(Noto Sans TC 2.004,Windows 11 內附;亦可從
  <https://fonts.google.com/noto/specimen/Noto+Sans+TC> 下載 `NotoSansTC[wght].ttf` 以 `--source` 指定)。
  授權 SIL Open Font License 1.1,宣告保留於字型 name table。
- 載入:`App/embeddedfonts.cpp`。兩平台都 `addApplicationFont`;**只有 WASM** 建 fallback 鏈
  (`QFont::insertSubstitutions` + Han/Common script fallback)。desktop 顯示不變。

## 開發用工具(dev-only)

- `App/e2epvdriver.h`:**只在 desktop 編入、只有設定 `TAIDAFLOW_E2E_PV_FILE` 才建立**。每 200 ms
  讀 `名稱=值` 行,經 setter 寫入 authoritative `Td` 名稱以 `Pv` 結尾的屬性(double 或 bool)。
  用途:無設備的開發機上,後端永遠不會更新 PV,E2E 以它模擬「desktop 端 PV 變化」。它不能寫 SV,
  也不碰 Manager/Modbus。`run-desktop.ps1 -PvFile <檔案>` 會設定環境變數。
- `scripts\desktop_input.py`(OS SendInput 操作真實 desktop UI)、`scripts\capture-window.ps1`
  (視窗截圖)、`serve_wasm.py --evidence-dir DIR`(僅此模式注入截圖/console 收集 shim)。
- `scripts\seed_history_sqlite.py`(**測試資料,dev-only**):app 未執行時,把已知的 N 筆 sensor 列寫進
  Core 自己建立的 `build\runtime-cwd\data\sensor_<yyyyMM>.sqlite`(只接受該路徑、表必須為空),
  下次啟動時由 Core 的真實讀取路徑(`Core::loadHistoryRecords` → SqlManager → `historyRecords` → Mirror)
  載入歷史頁;同時輸出畫面應顯示的值(`--out rows.json`)。無設備時 Core 永遠不會寫 sensor 列,
  所以歷史頁 / CSV 匯出只能這樣準備資料。它不走 Core 的寫入路徑(`saveSensorData`)。
- `scripts\compare_history_csv.py --fixture rows.json A.csv B.csv ...`:檢查匯出 CSV 的 BOM、標頭、
  列數、每格內容與 fixture 一致,且各檔位元組相同。
- `serve_wasm.py --evidence-dir DIR` 另會把頁面觸發的 Blob 下載(`<a download>`)複製一份存成
  `DIR\download-<檔名>`,並記錄 `showSaveFilePicker` 的呼叫與結果;網址加 `?evidence-download=anchor`
  時,在 app 載入前移除 `showOpenFilePicker/showSaveFilePicker`(等同 Firefox/Safari),讓
  Qt 走它自己的 `<a download>` 後備路徑(內建 browser pane 不支援原生存檔對話框時使用)。
- `desktop_input.py --title <視窗標題> click|key|type ...`:操作其他頂層視窗(例如 CSV 匯出的存檔對話框)。

## 歷史資料:時間區間與匯出(w2-041,`docs/taidaflow_history_export_spec.md`)

### 時間區間查詢(spec §2)

- 歷史頁按「篩選」/ Enter 發 `Td.historyRangeRequested(fromMs, toMs)`,「顯示全部」發
  `(0, 8640000000000000)`(不限區間)。單位 epoch 毫秒、本機時區日界、兩端都含(w2-040 契約)。
- Core(`Core::onHistoryRangeRequested`)寫回同步屬性 `historyRangeFromMs/ToMs`、回到第 1 頁
  (`historyCurrentPage` 不是 1 時設成 1,由它觸發載入),載入沿用 w2-039 的非同步 + 序號機制,每次只推送 10 筆。
  預設區間 = 啟動當月(Proxy 預設值)。區間與頁碼是**所有連線端共享**(與頁碼相同)。
- 秒換算:`ceil(fromMs/1000) .. floor(toMs/1000)`,且至少從 1 開始(timestamp <= 0 的列歷史頁本來就不顯示)。
- 跨月:`SqlManager::requestSensorHistoryRangePage` 只列出資料夾裡**實際存在**且與區間相交的
  `sensor_YYYYMM.sqlite`(不逐月走,「全部」不會掃幾百萬個月),新月份在前;總數 = 各月 COUNT 加總;
  頁面在月份間串接(每月內 `ORDER BY timestamp DESC, rowid DESC`)。各月 COUNT 有快取:月份檔大小與
  SQLite 檔頭 change counter 都沒變才沿用(有寫入的月份會重算)。

### 匯出(spec §3)

- 觸發:`Td.historyExportRequested(sessionId, fromMs, toMs)`(區間 = 目前查詢區間);取消:
  `Td.historyExportCancelRequested(sessionId)`。`sessionId` 是 `clientSessionId`(桌面 `desktop`,
  網頁 `web-xxxx`;只接受 `[A-Za-z0-9_-]{1,40}`)。
- 佇列(`HistoryExportManager`,主執行緒):**同時只跑一個**,其餘 FIFO;排隊中取消 → 直接移出;
  執行中取消 → 背景執行緒在下一段(<= 2000 列)停下,未完成檔刪除。同一 sessionId 已有排隊/執行中
  的匯出時再要求:**拒絕**(原本那筆的狀態與進度不動,只把它的 `message` 設成「已有匯出進行中,請取消後再試」,
  log 記 warning)。
- 狀態:`historyExportStatus[sessionId] = {state, progress, queuePosition, rowsWritten, totalRows,
  fileName, url, downloadPort, savedPath, message}`;`state` = queued / running / done / cancelled / error;
  `queuePosition` 1 = 下一個、執行中 0。進度只在「比上次多 >= 1 個百分點**且**距上次 >= 500 ms」時更新
  (每 1% 與每 500 ms 取較少者),狀態轉換立即更新。已結束的項目最多保留 30 筆。
- 生成(`HistoryExportWorker`,專用低優先權執行緒):用**自己的唯讀 SQLite 連線**讀月份檔(新月份在前),
  以 keyset 分段(每段 <= 2000 列,每段是一個獨立的短 statement,讀鎖只持有一段),邊讀邊寫
  (1 MB 緩衝)到 `QSaveFile`,完成才改名成正式檔名;取消 / 失敗時暫存檔丟棄。記憶體不隨檔案大小成長。
  SqlManager 執行緒(每秒存檔、歷史頁)不被匯出占用。
- CSV:UTF-8 BOM、CRLF、每格加雙引號;欄位 = `序號` + 歷史頁的 17 個標題(`時間`、TT-01..04、
  PT-01..07、FM-01、M1..M4,與 `Td.historyTitle` 同一份定義);順序同歷史頁(新到舊),序號 1..N;
  數值 = 歷史頁的換算(TT/M ×100/65535、PT ×1000/65535、FM-01 ×1),格式與頁面 `toFixed(2)` 相同
  (含剛好 .xx5 的進位規則);空值 `—`。檔名 `<sessionId>_<yyyyMMdd_HHmmss>.csv`(要求時間)。
- **桌面**(`desktop`):先跳「下載歷史資料」另存新檔對話框(預設「文件」資料夾 + 上述檔名),選定後
  同樣排隊/進度/可取消,直接寫到選定位置(不經暫存、不占名額),完成填 `savedPath`;對話框按取消 →
  `state = cancelled`(「已取消儲存」)。
- **網頁**:寫到 **`<desktop 工作目錄>\exports\`**(正式執行即 `build\runtime-cwd\exports\`;與 `data\`、
  `settings.sqlite` 同一個基準,重開 app 後連結仍有效)。完成時 `url = "/exports/<檔名>"`、
  `downloadPort = 8124`,網頁以 `http://<pageHost>:8124/exports/<檔名>` 下載(pageHost 由 main 的網頁端提供)。
  清理:每產生一個網頁檔後,若 `*.csv` 超過 20 個**或**總量超過 2 GB,依修改時間刪最舊的,直到兩個條件都
  滿足;不刪剛產生的檔,它單獨就超過 2 GB 時保留並記 log;正在被下載而刪不掉的檔記 log 跳過。
  啟動時刪除上次當機留下的暫存檔(`*.csv.XXXXXX`)。
- **下載服務**(`ExportDownloadServer`,專用執行緒上的 `QHttpServer`):綁 `0.0.0.0:8124`,只有
  `GET /exports/<檔名>`;檔名必須是 `<sessionId>_<yyyyMMdd_HHmmss>.csv`(不能含 `/ \ .. :`、不能是其他
  副檔名),且實際路徑必須在匯出資料夾內;回應 `Content-Type: text/csv; charset=utf-8`、
  `Content-Disposition: attachment; filename="<檔名>"`、`Access-Control-Allow-Origin: *`、
  `Content-Length`;檔案以 `QHttpServerResponder::write(QIODevice*)` 從磁碟分段送出(Qt 6.8 文件:
  非 sequential 裝置一次宣告全長、分段讀取,不整檔讀進記憶體)。其他路徑 404、壞檔名 400。
  綁定失敗只記 warning,app 繼續執行(網頁匯出檔仍會寫出)。
- 舊的 `Td.saveHistoryCsv()`(Q_INVOKABLE,前端組 10 筆 CSV)已沒有 QML 呼叫者;宣告在
  `TaidaFlowProxy.h`(main 分支負責),待 main 刪除。

## 測試 / 驗證(全部以 exit code 判定)

本 repo 沒有自己的單元測試(維護方專案);整合以下列可重跑檢查驗證:

```bat
:: 0. pack 與 1.0.1 來源逐檔一致、MANIFEST 24/24
python scripts\verify_pack.py --source ..\WebAssemblyTest\integration-pack\wasm-mirror
:: 0b. pack 自帶 native tests(WasmMirror.Engine / Runtime)
scripts\run-pack-tests.bat
:: 1. fresh 建置
scripts\build-desktop.bat fresh
scripts\build-wasm.bat wasm-release fresh
:: 2. WASM 不含後端(來源、Qt 模組、字串);desktop 含
python scripts\check_wasm_backend.py build\desktop build\wasm-release
:: 3. VERSION 地雷未觸發(兩邊 version_shadow_hits=0)
python scripts\check_version_shadow.py build\desktop build\wasm-release
:: 4. 字型子集涵蓋所有來源字元
python scripts\make_font_subset.py --check
:: 5. desktop 啟動(含安全探測、runtime-cwd、後端 + mirror + 8124 下載服務起來、關閉後 502/8124/8125 無殘留)
powershell -ExecutionPolicy Bypass -File scripts\verify-desktop-startup.ps1
:: 5b. (w2-041) 歷史區間 + CSV 匯出的 QTest(編譯真的 SqlManager / HistoryExport / Proxy 原始碼):
::     先做 30 天、兩個月份檔的測試資料(build\w2-041-bench),再建置並跑 CTest(需 8124 空著)
python docs\evidence\w2-041\tools\make_bench_db.py
docs\evidence\w2-041\tools\run-qtest.bat
::     30 天匯出檔逐列比對(獨立的 Python 實作)
python docs\evidence\w2-041\tools\verify_export_csv.py "build\w2-041-qtest\work\exports_30d\web-t30d_*.csv" --data build\w2-041-bench\data --from 1786896000 --to 1789487999
:: 6. desktop 逐像素不退步(基準 = dc91f01 原始碼,scripts\build-baseline.bat 可重建)
python scripts\image_diff.py docs\evidence\wasm-v4\02-baseline-dc91f01-main.png docs\evidence\wasm-v4\05-after-desktop-main.png --mask 0,0,1942,45 --mask 1760,55,1942,110 --tolerance 2
```

E2E 證據(雙向同步 double/bool、唯讀 PV 單向、斷線離線提示與控制項停用、重連恢復、
中文顯示)在 `docs/evidence/wasm-v4/`,檔名即步驟說明。CSV 匯出(desktop 存檔、網頁下載、
離線匯出、內容比對)在 `docs/evidence/wasm-v4-csv/`。
