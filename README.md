# TaidaFlow

> **編譯(給開發人員):[docs/BUILD.md](docs/BUILD.md)**
> ——從 clone、要裝的軟體與版本、工具路徑,到桌面版 / 網頁版的腳本與不用腳本的手動編譯、Qt Creator、編譯後必跑的檢查、
> main → core 合併後要做的事、常見問題。
>
> **部署與啟動(正式機 / 測試機,給操作人員):[docs/DEPLOY_AND_STARTUP.md](docs/DEPLOY_AND_STARTUP.md)**
> ——打包內容、第一次部署逐步做法、日常操作(TaidaFlow 與 nginx 的啟動 / 停止)、更新版本、**完全不用腳本的手動部署**、
> config.json 欄位表、port 與防火牆、常見問題。打包時複製成打包資料夾的 `DEPLOY.md`。部署**不需要 Python**
> (網頁版的中文字型已經編進程式)。

TaidaFlow 是 Qt Design Studio 產生的 Qt Quick HMI(主畫面 / 警報 / 歷史)。本分支 `core`
含真實後端 `Core/`:`core.cpp`、`manager.cpp`、`Modbus_Client`(5 台 ADAM,位址在 **config.json**,廠區預設
`192.168.1.201~205:502`,會寫 DO/AO;**測試用**環境變數 `TAIDAFLOW_DEVICE_PROFILE=simulator`
改連本機模擬器 `127.0.0.201~205`,見「接 Adam60xxSimulator」)、`Modbus_Server`(config.json `modbusServer`,預設 `0.0.0.0:502`)、
`Ms300FaultReader`(Modbus RTU,config.json `devices.ms300`,預設 `COM2`)、`RESTManager`(REST API:config.json `rest`,
預設 `127.0.0.1:18080`,區網經 nginx `http://<IP>/api/...`,見「REST API」)、`SqlManager`(SQLite)、
`HistoryViews`(各連線端獨立的歷史檢視)與 `HistoryExport`(歷史 CSV 匯出佇列,兩者見「歷史資料:時間區間與匯出」)、`AlarmViews`(各連線端獨立的警報頁檢視,見「警報頁:各連線端獨立的時間區間與分頁」)、`AppHttpServer`(可重用的
HTTP 伺服器單例,config.json `http`(預設 `0.0.0.0:8124`)同時提供**網頁**與 **CSV 下載**,見「網頁與下載(HTTP 8124)」)。

**設定檔 config.json(w2-062)**:所有現場會不一樣的設定(資料資料夾、設備位址、各服務 port、nginx)都在程式旁的
`config.json`,沒有時程式自動建立預設檔;開發機用 `deploy\dev\config.dev.json`(`scripts\run-desktop.ps1` 以
`TAIDAFLOW_CONFIG` 指定)。說明:[docs/BUILD.md §10](docs/BUILD.md)、欄位表:[DEPLOY_AND_STARTUP.md §6](docs/DEPLOY_AND_STARTUP.md)。

## port 一覽(預設值,全部可在 config.json 改)

| port | 程式 | 用途 | 對外(防火牆) |
|---|---|---|---|
| **80** | nginx(另一個程式;打包資料夾附在 `nginx\`) | 網頁、`/runtime.json`、網頁同步 `/mirror`(WebSocket)、CSV 下載 `/exports/`(續傳)、REST `/api/` | **開** |
| **502** | TaidaFlowApp(Modbus TCP 伺服器) | 外部 HMI / SCADA 讀寫 | **開**(不限來源) |
| 8124 | TaidaFlowApp(`AppHttpServer`) | 網頁與下載的備援(nginx 沒開時) | 不開 |
| 8125 | TaidaFlowApp(區網轉發 `App/lanrelay.h`) | 網頁同步的備援(nginx 沒開時) | 不開 |
| 18125 | TaidaFlowApp(Mirror 伺服器,只綁 127.0.0.1) | 內部;nginx `/mirror` 與 8125 都轉到這裡 | 不開 |
| 18080 | TaidaFlowApp(REST,只綁 127.0.0.1) | 內部;nginx `/api/` 轉到這裡 | 不開 |

正式機的 Windows 防火牆**只開 80 與 502**(Mango 決定);其他都是內部或備援(DEPLOY_AND_STARTUP.md §7)。

QML 只面對 `Core/TaidaFlowProxy.h`(QML singleton `Td`,module URI `TaidaFlowBackend`)。
透過 `integration-pack/wasm-mirror`(pack **1.0.1**,wire protocol 3,**唯讀、由維護方提供**,
整包複製,`MANIFEST.sha256` 24/24),同一份程式可編成:

- **Desktop(Windows MSVC)**:authoritative 端。`Core::instance().init()` 建立並擁有
  `TaidaFlowProxy`、啟動全部硬體後端,Mirror server 綁 `ws://127.0.0.1:18125/mirror`(內部 port,
  只限本機);區網的網頁經 nginx 80 的 `/mirror` 轉給它,nginx 未啟用時的備援是 `App/lanrelay.h` 在 `0.0.0.0:8125`
  的轉發(見「區網連線(mirror)」)。
- **WebAssembly**:replica 端。**不編譯任何後端**(Desktop-only Core,pack 文件
  package-integration §1/§5.4、guide §10/§11),`main.cpp` 直接建立 `TaidaFlowProxy` 當 replica,
  瀏覽器頁面連回**載入頁面的那台主機**(`location.hostname`),port 取自 `/runtime.json` 的 `mirrorPublicPort`
(nginx 啟用時 = `nginx.port`,經 nginx `/mirror`;否則 = `mirror.publicPort` 8125),雙向同步 `Td` 的全部 Q_PROPERTY。

## 安全注意(開 desktop 前必讀)

desktop 版會對 config.json 的 5 台 ADAM(廠區預設 `192.168.1.201~205:502`)建 Modbus TCP 連線並**寫入** DO/AO
(泵浦、閥、變頻器、緊急停止迴路),開 MS300 的序列埠(預設 `COM2`),並開 Modbus server(預設 `0.0.0.0:502`)。
在非現場的開發機上:

- 一律用 `scripts\run-desktop.ps1` 啟動(設定檔預設 `deploy\dev\config.dev.json`,`-Config` 可換)。它先用**同一份設定檔**跑
  `scripts\safety_probe.ps1`:對「實際生效的設備位址」(config.json 的位址,或 simulator profile 換成的 `127.0.0.201~205`)
  非本機位址只做 TCP connect(1.5 秒逾時、**不送任何 Modbus**),本機位址則必須由 `Adam60xxSimulator.exe` 在聽;列出本機序列埠;
  檢查 config.json 的各服務 port。任一裝置可達、設定的 MS300 序列埠存在、或 `modbusServer.port` 被別人占用 → **不啟動**(exit 3);
  `http.port`、`mirror.publicPort`、`mirror.internalPort`、`rest.port` 已被占用 → 不啟動(exit 4 `BUSY`,不會關掉別人的程式);
  模擬器還沒起來 → exit 5;config.json 不能用(JSON 格式錯誤等)→ exit 2(`CONFIG-ERROR`);`nginx.port` 只列出、
  **不阻擋**(nginx 只送檔 / 轉送,不碰設備)。每次探測附加寫入
  `build\runtime-logs\safety-probe.log`(simulator 模式 `safety-probe-sim.log`;`-LogFile` 可改)。
- desktop 另開 HTTP 服務(`http`,網頁 + CSV 下載)、mirror 區網轉發(`mirror.publicBind:publicPort`)與內部 Mirror
  (`127.0.0.1:mirror.internalPort`)、REST API(`rest`,只限本機;區網經 nginx 的 `/api/`,可用 PUT 改設定,不做存取控管,見「REST API」)。
- 工作目錄 = config.json 的 `dataDir`(開發設定是 `build\runtime-cwd\`,程式啟動時自己切換):Core 在這裡寫 `TaidaFlowSettings.ini`、
  `settings.sqlite`、`data\sensor_YYYYMM.sqlite`(及 REST 用的 `device_info.ini`),網頁匯出的 CSV 寫在 `exports\`。
  `build/` 已被 `.gitignore` 排除,work tree 保持乾淨。
- 不要為了接模擬器改網路設定(不加 IP alias)或改 Core 程式;要接模擬器請用下方
  「接 Adam60xxSimulator」的測試用 profile 或 `deploy\dev\config.simulator.json`。`run-desktop.ps1` 在預設模式會**移除**
  `TAIDAFLOW_DEVICE_PROFILE`,確保 app 用的就是探測過的位址。

## 建置

完整說明在 **[docs/BUILD.md](docs/BUILD.md)**(需要的軟體與版本、安裝與勾選項目、工具路徑與環境變數、專案 / CMake 結構、
桌面版與網頁版的腳本做法與不用腳本的手動做法、Qt Creator、編譯後必跑的檢查、main → core 合併後要做的事、常見問題)。
這裡只留最常用的兩行,其餘以 BUILD.md 為準:

```bat
scripts\build-desktop.bat [fresh]                          :: -> build\desktop\TaidaFlowApp.exe
scripts\build-wasm.bat [wasm-release|wasm-debug] [fresh]   :: -> build\wasm-release\TaidaFlowApp.html / .js / .wasm
```

工具不在預設位置(`C:\Qt\6.8.3`、`C:\Qt\Tools`、VS 18 的 `vcvars64.bat`、`C:\tools\emsdk`)時,先設 `TAIDAFLOW_QT_ROOT`、
`TAIDAFLOW_QT_TOOLS`、`TAIDAFLOW_VCVARS64`、`TAIDAFLOW_EMSDK`(BUILD.md §2.6)。

## 執行(desktop + 瀏覽器)

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1                     # 先部署網頁 -> build\desktop\web(含 runtime.json)
powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -Label "manual"   # 再探測並啟動(deploy\dev\config.dev.json)
# nginx:cd build\desktop\nginx 後 start nginx(BUILD.md §4.5);瀏覽器開 http://127.0.0.1/
# 沒有 nginx 時的備援:http://127.0.0.1:8124/TaidaFlowApp.html(設定檔要 nginx.enabled=false,網頁才會連 8125 同步)
```

- 網頁由 nginx(port 80,見「nginx 網頁前端與下載」)或 **desktop 自己**(`AppHttpServer`,與 CSV 下載同一個 `http.port`)提供。
  網頁檔資料夾的決定順序見「網頁與下載(HTTP 8124)」。**先 `deploy-web.ps1` 再開 app**:app 啟動時才決定網頁資料夾,
  `build\desktop\web` 還不存在(例如剛做完 fresh 建置)時會退回 `build\wasm-release`,把 `runtime.json` 寫進建置輸出,
  而 nginx(`build\desktop\nginx`,網頁根目錄 `../web`)送的是 `build\desktop\web`(DEPLOY_AND_STARTUP.md §9.2 步驟 1c)。
  正式部署同樣是 `<exe 資料夾>\web`。
- **網頁怎麼知道同步用哪個 port**(規格 §3):程式啟動時把 `runtime.json`(`{"mirrorPublicPort":<port>,"version":1}`)寫進網頁資料夾,
  nginx 與 8124 都當靜態檔送出(`Cache-Control: no-store`)。`nginx.enabled` 為 true 時 port = `nginx.port`(網頁經 nginx 的
  `/mirror` 同步,正式機防火牆只開 80),否則 = `mirror.publicPort`(8125 的區網轉發)。網頁讀不到時退回 8125。
- **log**(w2-064 / w2-065,規格 §2 `log`;詳見 `docs/DEPLOY_AND_STARTUP.md` §13):程式自己寫檔到 config.json 的 `log.dir`
  (相對於 `dataDir`,預設 `C:\TaidaFlowData\logs`;開發設定 = `build\runtime-cwd\logs`):`taidaflow-YYYY-MM-DD.log`
  (warning 以上,保留 60 天)與 `taidaflow-YYYY-MM-DD-full.log`(全部,保留 7 天),換日換檔並清理,只刪符合這兩種檔名的檔;
  config.json 讀不到時寫到 config.json 旁的 `logs\`。正式機啟動 / 停止腳本在同一資料夾寫 `launcher-YYYY-MM-DD.log`,
  nginx 寫 `nginx-access-YYYY-MM-DD.log` 與 `nginx-error.log`;`start-taidaflow` 啟動時依 `log.quiet.keepDays` 清理
  launcher / nginx access log。舊版的 `taidaflow-yyyyMMdd-HHmmss.log` 不刪。測試:`App/tests` 的 `tst_applog`、
  `docs\evidence\w2-065\tools\test-config-reader.ps1`(腳本端的 log 資料夾與清理)。
- `run-desktop.ps1`(開發用)設 `QT_FORCE_STDERR_LOGGING=1`、PATH 加 Qt bin、`TAIDAFLOW_CONFIG=<設定檔>`,另把程式輸出存到
  `build\runtime-logs\`(程式自己的 log 檔照樣寫到 `build\runtime-cwd\logs`)。啟動 log 應含 `[Config] ...`(每一項設定的值與來源)、`[Config] Core ...`(後端用的值)、
  `[ModbusServer] listening on 0.0.0.0:502 unit=1`、`WASM Mirror endpoint: ws://127.0.0.1:18125/mirror`、
  `LAN relay listening: 0.0.0.0:8125 -> 127.0.0.1:18125`、`[Web] runtime.json written: ... = {"mirrorPublicPort":80,"version":1} (...)`、
  `[Web] web page folder (<來源>): <資料夾> -> http://<host>:8124/TaidaFlowApp.html` 與
  `[Web] HTTP service listening on 0.0.0.0:8124`(被占用時只記 warning,app 照常執行)。
- 瀏覽器 console:`/runtime.json: mirrorPublicPort = 80` 與 `WASM Mirror ready: true` 即同步完成。

### 網頁載入畫面(w2-058,轉場 w1-066)

- 樣板:`App/wasm/TaidaFlowApp.shell.html`(純 HTML/CSS/JS,重現 CubeLoader 旋轉發光立方體 + 「TAIDAFLOW」
  與繁中狀態文字;不引用任何外部 CSS/JS/字型、不用函式庫)。
- **載入完成後的轉場(w1-066)**:`onLoaded` 不再立刻隱藏載入頁,而是先等 App 的畫面真的畫出來——兩次
  `requestAnimationFrame` 再加 250 ms(分頁在背景、rAF 不來時,1 秒保底也會開始)——再播約 800 ms 的轉場:
  狀態文字改成「準備完成」,立方體加速旋轉並放大、中心光暈擴散後淡出,文字與地面陰影先淡出(240 ms),
  整個載入層在 240~800 ms 淡出(轉場一開始就不再攔截點擊);App 畫面在 200~800 ms 由透明、略縮小淡入。
  兩者的 `animationend` 都到才收尾(隱藏載入層、移除轉場用的 class 與 `will-change`),沒收到事件時 1.5 秒保底收尾。
  onLoaded 到畫面完全接手約 1.1 秒。只動 opacity / transform,沒有 filter。
- **減少動態效果**(瀏覽器 / 系統設定 `prefers-reduced-motion: reduce`):等待階段相同,之後只做 200 ms 的淡出淡入
  (立方體不旋轉、不放大、沒有光暈)。
- **錯誤時**:載入失敗、瀏覽器不支援 WebAssembly、JavaScript 關閉與程式結束時停在載入頁顯示繁中訊息(紅字)。
  若錯誤發生在轉場等待中或播放中,轉場立即中止(計時器、rAF、事件監聽全部取消),載入層回到完全不透明、立方體停住並顯示
  紅字;轉場結束後才發生的程式結束與以前相同,載入頁重新出現在畫面上方顯示訊息。
  w2-084 起,紅字下方多一行「N 秒後自動重新整理頁面…」,10 秒後(或退避規則要求的更久)自動重新整理,見「網頁斷線偵測與自動恢復」。
- 套用:Qt 6.8 沒有自訂 HTML shell 的 CMake 參數,它在 CMake configure 時從自己的 `wasm_shell.html`
  產生 `build\<wasm preset>\TaidaFlowApp.html`。`App/CMakeLists.txt` 在同一次 configure、Qt 產生之後
  立刻呼叫 `App/wasm/apply_wasm_shell.cmake`,以 Qt 產生的頁面取 `@APPNAME@`/`@APPEXPORTNAME@`/
  `@PRELOAD@` 的值填入樣板並覆蓋;configure log 會印 `[wasm-shell] ...`(值與 SHA-256)。只影響 WASM,
  桌面版不變。`qtlogo.svg` 仍由 Qt 複製、仍會被 deploy-web 部署,新頁面不再使用(留著無影響)。
- 修改:改樣板 → `scripts\build-wasm.bat`(樣板是 configure 相依,改了會自動重跑 configure)→
  `powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1`(nginx 執行中靠 ETag 重新驗證,不必重啟)。靜態檢查
  (不開瀏覽器):`findstr /c:"TAIDAFLOW" build\wasm-release\TaidaFlowApp.html` 有結果、`findstr /c:"@APPNAME@" ...` 沒有結果。
- main 分支的 wasm 建置仍用 Qt 預設頁;要一致需把 `App/wasm/` 與 `App/CMakeLists.txt` 的 w2-058 區塊
  一起帶到 main(main → core 合併時這兩處只在 core,不會被覆蓋,除非 main 也改了同一段)。

### 網頁與下載(HTTP 8124,w2-049)

desktop 的 Core 只透過 `AppHttpServer::instance()`(`Core/AppHttpServer/`,只依賴 Qt Core/Network/
HttpServer 的可重用類別,用法見該資料夾的 `README.md`)提供 HTTP,單例跑在自己的執行緒
(`AppHttpServerThread`),大檔傳送不占 UI 執行緒。`Core::startHttpServer()` 在 `Core::init()` 掛上網頁
(`/`)後以 config.json 的 `http.bind` / `http.port`(預設 `0.0.0.0:8124`)`start`;`HistoryExportManager` 掛上 `/exports`;
關閉時由 `Core::shutdown`(`aboutToQuit`,w2-067,見「關閉流程」)在匯出之後 `stop()`(join 它的執行緒)。

- **網頁檔資料夾**(第一個含 `TaidaFlowApp.html` 的):環境變數 `TAIDAFLOW_WEB_DIR` →
  `<exe 資料夾>\web` → 開發預設 `<exe 資料夾>\..\wasm-release`(即 `build\wasm-release`;w2-062 起是相對路徑,
  程式裡不再有建置機的絕對路徑;不存在就跳過)。找到網頁資料夾後寫入 `runtime.json`(內容不同才寫;不能寫只記 warning,網頁退回 8125),
  並以 `StaticOptions::fileCacheControl` 讓它帶 `Cache-Control: no-store`(其他網頁檔規則不變)。
  每個候選與結果都記 log(`[Web] candidate ...`、`[Web] web page folder (...)`);都找不到只記
  warning(`[Web] no web page folder found ...`),`/exports` 下載與其他功能照常。
- **部署**:`powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1 [-Source build\wasm-release]
  [-ExeDir build\desktop]` 清空 `<exe 資料夾>\web` 後複製同一次 build 的網頁檔(不含 CMake/Ninja 檔),
  並為 `.wasm/.js/.html/.svg` 等產生 `.gz`(修改時間與原檔相同)。
- **行為**(取代舊的開發用網頁伺服器,符合 `integration-pack/wasm-mirror/docs/http-server-requirements.md`):
  - MIME:`.html` `text/html; charset=utf-8`、`.js/.mjs` `text/javascript; charset=utf-8`、
    `.wasm` `application/wasm`、`.css`、`.json`、`.svg`、`.png`、`.ico`、`.ttf/.woff2` 等;
    網頁掛載只送網頁類型的副檔名(開發預設就是 build 資料夾,`CMakeCache.txt` 等一律 404)。
  - 快取:每個檔帶 `ETag`(大小 + 修改時間)與 `Last-Modified`,`Cache-Control: no-cache`(瀏覽器每次
    重新驗證);`If-None-Match` / `If-Modified-Since` 相符 → 304;重新建置後 ETag 變 → 200 拿新檔,
    HTML/JS/WASM 不會新舊混用。
  - gzip:有 `<檔名>.gz`、不比原檔舊、且 `Accept-Encoding` 含 gzip → 送 `.gz`(`Content-Encoding: gzip`、
    `Vary: Accept-Encoding`、MIME 仍為原檔;實測 `.wasm` 33,828,575 → `.gz` 12,447,241 bytes,37%)。
  - `Cross-Origin-Opener-Policy: same-origin`、`Cross-Origin-Embedder-Policy: require-corp`、
    `Cross-Origin-Resource-Policy: same-origin`(與舊伺服器相同,main 的 multithread 版也可用)。
  - `/` → 302 `/TaidaFlowApp.html`;不列目錄;支援 `HEAD`;`GET`/`HEAD` 以外 405。
  - 路徑穿越一律 400/403/404:`..`、`%2e%2e`、`%2f`/`%5c`/反斜線、磁碟代號與 `:`、`//`、結尾 `.`/空白、
    裝置名(`NUL`、`CON` …)、`.` 開頭的隱藏檔,以及網頁資料夾**底下**的符號連結 / junction(不跟隨)。
- 下載網址:`http://<pageHost>:<downloadPort>/exports/<檔名>`(main 的 QML / Proxy 不需改)。
  `downloadPort` = config.json 的 `nginx.enabled ? nginx.port : http.port`(預設 80,由 nginx 送檔,見下一節);
  環境變數 `TAIDAFLOW_DOWNLOAD_PORT` 可臨時覆寫(log 記 environment override)。8124 的 `/exports` 一直保留,作為備援。

### nginx 網頁前端與下載(port 80,w2-050 / w2-057 / w2-062)

nginx 是**另一個程式**,做網頁前端:從部署好的網頁資料夾送網頁與 `runtime.json`,把網頁同步 `/mirror`(WebSocket)轉給
app 的 Mirror(`127.0.0.1:mirror.internalPort`)、`/api/` 轉給 REST(`127.0.0.1:rest.port`),並**直接從 app 的匯出資料夾**送歷史
CSV(下載不經過 Qt、支援續傳)。正式機防火牆只開 80(與 502),所以網頁的全部 HTTP 都走 nginx;app 內建的 8124 / 8125
保留作 nginx 沒開時的備援。

- `http://<IP>/`(nginx,`nginx.port` 預設 80,`/` → 302 `/TaidaFlowApp.html`)
- `http://<IP>:8124/TaidaFlowApp.html`(desktop app 內建 `AppHttpServer`,備援)

**設定檔 = 一份樣板、一支產生程式**(w2-062):`deploy/nginx/taidaflow.conf` 是樣板,六個記號(網頁根目錄、匯出資料夾、
log 資料夾(w2-065)、nginx port、REST port、Mirror port)全部由 config.json 帶入(記號表見 DEPLOY_AND_STARTUP.md §5.4)。產生程式只有一支:`scripts\install-nginx-config.ps1`,它寫出完整的
`<nginx 資料夾>\conf\nginx.conf`(網頁根目錄寫成相對於 nginx 資料夾的 `../web`,整個安裝資料夾搬家也有效;檔頭記錄產生程式版本、
使用的 config.json 路徑 / 修改時間 / SHA-256 與安裝資料夾),再以預設前綴執行 `nginx -t`。三種用法:

| 情境 | 怎麼產生 | 怎麼啟動 / 停止 |
|---|---|---|
| **正式機**(打包資料夾附 `nginx\`,nginx 1.30.5 + 授權檔) | 一次性:`install-nginx-config.ps1`(讀 `<安裝資料夾>\config.json`,舊的非 TaidaFlow `nginx.conf` 先備份成 `nginx.conf.orig-<時間>`);`start-taidaflow` 每次啟動都用同一支程式檢查,檔案不存在、不是這個安裝資料夾 / 這份 config.json 產生的就重產(自動產生的舊檔留成 `.prev-<時間>`,手寫的留成 `.orig-<時間>`),nginx 執行中則 `nginx -s reload` | 標準做法:`cd <安裝資料夾>\nginx` → `start nginx`(`nginx -s reload` 套用、`nginx -s quit` 停止)。`start-taidaflow` 在 nginx 沒執行時用同樣方式啟動;`stop-taidaflow` **不停** nginx |
| **桌面版建置**(BUILD.md §4.5) | CMake target `taidaflow_nginx_conf`:`build\desktop\nginx\conf\nginx.conf`(`TAIDAFLOW_NGINX_CONFIG`,預設 `deploy\dev\config.dev.json`),並從 `TAIDAFLOW_NGINX_DIR` 複製 `nginx.exe`、`conf\mime.types` | `cd <repo>\build\desktop\nginx` → `start nginx` / `nginx -s quit` |
| **開發機腳本** | `scripts\nginx-web.ps1`(`nginx-start.ps1` / `nginx-stop.ps1` 是 `-Action start|stop` 捷徑,另有 `reload`、`test`、`status`)用同一個函式把樣板寫到**獨立前綴** `build\nginx\conf\taidaflow.conf`,不動任何 nginx 安裝資料夾 | 以 `nginx -p <build\nginx>/ -c <build\nginx>/conf/taidaflow.conf`(完整路徑)啟動;停止只停本腳本起的那個(狀態檔的 pid + 映像檔路徑 + 啟動時間 + `logs\nginx.pid` 都要相符,`nginx -s quit`) |

- 開發機腳本的值:`-Config`(否則 `TAIDAFLOW_CONFIG`,否則 `deploy\dev\config.dev.json`)的 `nginx.port`、`rest.port`、
  `mirror.internalPort`、`<dataDir>\exports`、`nginx.exe`、`log.dir`;`-Port`、`-RestPort`、`-MirrorPort`、`-ExportDir`、`-Nginx` 可單次覆寫,
  另有 `-WebRoot`(預設 `<exe 資料夾>\web`)、`-ExeDir`、`-RuntimeDir`(預設 `build\nginx`)、`-TimeoutSec`(預設 20)。
  start / reload / test 時順便把網頁資料夾的 `runtime.json` 寫成 nginx 實際的 port。
- `nginx.exe` 的位置:config.json 的 `nginx.exe`(相對路徑以 config.json 所在資料夾為準;沒寫時 `nginx\nginx.exe`,即打包附的那個)。
  開發設定 `config.dev.json` 指向 `C:\tools\nginx\nginx-1.30.5\nginx.exe`,不同就複製一份改(docs/BUILD.md §2.9)。
- 腳本 exit code(nginx-web.ps1):0 完成;1 沒有本腳本起的 nginx 在跑(stop/reload/status);2 找不到 nginx /
  網頁資料夾 / `TaidaFlowApp.html` / config.json 不能用,或路徑含設定檔不能用的字元;3 `nginx -t` 失敗(不啟動);4 port 已被
  占用(不啟動,也不關掉占用者)或已在跑;5 網頁資料夾或匯出資料夾是/含有符號連結、junction;6 起來但時限內沒 listen(已停掉);
  7 停止失敗。`install-nginx-config.ps1`:0 寫好且 `nginx -t` 通過;2 缺檔 / config.json 不能用;3 `nginx -t` 失敗;4 寫好但
  nginx.port 被別的程式占用(列出占用者,不關);5 有連結 / junction(不寫)。
- **安裝 nginx**(開發機一次,repo 外):步驟(nginx.org 下載 **Stable** 版 Windows zip 與 `.asc`、以 Git for Windows 內附的 gpg
  驗簽、SHA-256、解壓到 `C:\tools\nginx\nginx-<版本>\`、`nginx -v` 驗證)、寫死這個路徑的地方與裝在別處時怎麼改,
  一律見 **[docs/BUILD.md §2.9](docs/BUILD.md)**(w2-077 起以那裡為準)。目前用 **1.30.5**(SHA-256
  `e5afe28b6a50bec92c478bfe1a4d3758206b80fb77159277bc5c4e88955c2a35`,2,776,622 bytes,
  簽章者 Sergey Kandaurov `D6786CE303D9A9022998DC6CC8464D549AF75C0A`;證據 `docs/evidence/w2-050/01-*`)。
  打包時 `package-release.ps1` 附上這一份(`-NginxDir` 可換)。
- 網頁規則與 app 的 8124 相同:MIME(`.wasm` = `application/wasm`、`.html` / `.js` 帶
  `charset=utf-8`)、只送網頁檔副檔名(`html js mjs wasm css json map svg png jpg ico ttf otf woff
  woff2`,其他如 `CMakeCache.txt`、`build.ninja`、`.gz` 本身 → 404)、`.` 開頭 404、不列目錄、
  `/` → 302 `/TaidaFlowApp.html`、`etag on` + `Cache-Control: no-cache`(HTML 與其他檔都每次重新
  驗證,新 build 一定拿到新版)、`gzip_static on` 送 `deploy-web.ps1` 產生的 `.gz`(`Vary:
  Accept-Encoding`)、COOP `same-origin` / COEP `require-corp` / CORP `same-origin`、
  `server_tokens off`(錯誤頁只有 `nginx`,不含版本與路徑)。`/runtime.json` 例外:`Cache-Control: no-store`、不送 `.gz`、
  不做 ETag。`/mirror`:WebSocket 轉送(`proxy_http_version 1.1`、`Upgrade` / `Connection` 標頭、讀寫逾時 3600 秒、`proxy_buffering off`)。
  路徑穿越由 nginx 先正規化(`..`、`%2e`、`\`)再比對,離開根目錄 → 400。
- **Windows 版 nginx 沒有 `disable_symlinks`**(nginx.org 文件:只在有 `openat()`/`fstatat()` 的系統;
  1.30.5 `nginx -t` 回 `unknown directive`),會跟隨網頁資料夾裡的 junction(實測見
  `docs/evidence/w2-050/live/`)。所以 `install-nginx-config.ps1` 與 `nginx-web.ps1` 會掃描網頁資料夾與匯出資料夾,有
  reparse point(符號連結、junction)就拒絕(exit 5);`deploy-web.ps1` 也不會刪除含連結的舊網頁資料夾
  (避免 `Remove-Item` 跟著刪到目標),複製後再掃一次。nginx 執行中才建立的連結掃不到:`/exports` 內名稱
  像匯出檔的**資料夾**(junction)一律 404;檔案符號連結需要系統管理員權限才能建立,不在防護範圍。
- nginx 在 Windows **不是服務**,開機不會自己起來;本專案不註冊服務。正式機由 `start-taidaflow`(nginx 沒在跑時)以
  `start nginx` 的方式帶起來,開機自動啟動用打包資料夾的 `register-autostart.ps1`(工作排程器「使用者登入時」,先 `-WhatIf`)。
- Windows 首次有程式 listen `0.0.0.0` 時可能跳出防火牆詢問視窗;腳本不改防火牆。

**下載(`/exports/`,nginx 直送)**:

- 只送 `^/exports/[A-Za-z0-9_-]{1,40}_\d{8}_\d{6}\.csv$`(與 Core 的 `isValidExportFileName` 相同),
  其他(子資料夾、別的副檔名、`..`、編碼過的斜線、太長的 sessionId …)一律 404。回應
  `Content-Type: text/csv; charset=utf-8`、`Content-Disposition: attachment; filename="<檔名>"`、
  `Cache-Control: no-store`、`Access-Control-Allow-Origin: *`,從磁碟串流(nginx 與 app 記憶體不隨檔案
  大小成長,實測見證據)。
- Core:`historyExportStatus` 的 `downloadPort` = config.json 的 `nginx.enabled ? nginx.port : http.port`(網頁連結
  `http://<pageHost>:80/exports/<檔名>`,與不帶 `:80` 等價);環境變數 `TAIDAFLOW_DOWNLOAD_PORT=<1..65535>` 可臨時覆寫
  (正式機的 `start-taidaflow` 在 nginx 起不來時改以 `nginx.enabled=false` 啟動 app,連結就改走 `http.port`)。
  log:`[Config] Core download links -> port 80 (derived ...)` 與 `[Export] web export folder ... download links -> port 80`。
- **續傳(HTTP Range)**:`Accept-Ranges: bytes`、`ETag`、`Last-Modified`;`Range` → 206 +
  `Content-Range`;`If-Range` 與目前 ETag 相符才 206,不符(檔案換了)→ 200 整檔;`/exports` 不開 gzip、
  不經 proxy,也沒有其他會改內容的 filter。`max_ranges 1`:一次只允許一個區段(續傳只需要一段);多區段
  要求改回 200 整檔,不產生 multipart 回應(避免用大量小區段放大負載)。
  - 續傳的前提是檔案**還在**匯出資料夾:清理規則(超過 20 個或 2 GB 刪最舊的)可能已把它刪掉,之後的
    續傳 / 重新下載會 404。
  - **8124(`AppHttpServer` 備援)不支援續傳**:`Range` 被忽略,一律 200 整檔。
- 清理遇到 nginx 正在送的檔:Core 照舊用 `QFile::remove`,刪不掉就記 log 跳過、下次再刪。Windows 上
  nginx 1.30.5 開檔時允許刪除(`FILE_SHARE_DELETE`),實測刪除**成功**,傳送中的下載仍完整送完(同一個
  SHA-256),之後的新要求 404(`docs/evidence/w2-050/live/summary.txt` 的 D7 (d) 段)。

### REST API(`RESTManager`,w2-060)

原後端開發者的 `Core/RESTManager.{h,cpp}`(QHttpServer)自 w2-060(Mango 2026-09-28)起由 `Core::init()` 建立並啟動
(`Core::startRestServer()`,在 `startHttpServer()` 之後)。

- **位址**:config.json 的 `rest.bind` / `rest.port`,預設只綁 `127.0.0.1:18080`(內部 port,區網位址連不到;`rest.bind`
  不是本機位址時 log 記 warning);區網一律經 nginx:`http://<IP>/api/...`
  (`deploy/nginx/taidaflow.conf` 的 `location ^~ /api/` 原樣轉給 `http://127.0.0.1:<rest.port>`,方法、body、query、
  標頭都保留;`http://<IP>/api/` 本身轉到 REST 的狀態 route `/`)。w2-062 起**不再讀**環境變數 `TAIDAFLOW_REST_PORT`;
  nginx 設定與程式讀同一份 config.json,port 一定一致(正式機 `start-taidaflow.ps1 -RestPort` 仍可單次覆寫,
  會一起重產 nginx.conf)。18080 選擇理由:開發機上未被占用、不在
  Windows 保留範圍(`netsh int ipv4 show excludedportrange protocol=tcp` 只有 2869、50000-50059),也不與
  80/502/8124/8125/18125 衝突。
- **log**:`[Config] Core REST API (rest) -> 127.0.0.1:18080 (...)`、`[REST] REST API listening on 127.0.0.1:18080 (...)` 與每個
  route 一行 `[REST] route GET,PUT,OPTIONS /api/...`(表格在 `Core/core.cpp` 的 `kRestRoutes`,
  `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-rest-routes.ps1` 比對它與 `RESTManager.cpp`
  實際註冊的 route,不一致 exit 1);RESTManager 自己另印 `Device SN: ...` 與 `Running on http://127.0.0.1: 18080 /`。
  綁定失敗(port 被占用)只記 `[REST] REST API NOT started on 127.0.0.1:18080 ...`,app 照常執行,nginx 的 `/api/` 回 502。
  關閉:`Core::shutdown`(`aboutToQuit`,w2-067)在停止設備連線與 Modbus 伺服器之後刪除 RESTManager
  (listener 關閉,log `[REST] REST API stopped`)。
- **工作目錄的檔案**:RESTManager 啟動時(與每次 `GET /api/device/sn`)讀工作目錄的 `device_info.ini`
  (`[device] sn`);**不存在就建立**並寫入 `sn=sn000000`。工作目錄不能寫時 QSettings 只是寫不進去,仍回
  `sn000000`,不會崩潰。其餘資料來自 `SqlManager`(`settings.sqlite`、`data\sensor_YYYYMM.sqlite`)。
- **存取控管**:**沒有**(內網,Mango 決定)。任何連得到 nginx(port 80)的人都能讀歷史資料並用 PUT 改下表標「會改」的設定。
  CORS 由 RESTManager 自己加(`Access-Control-Allow-Origin: *`、`Allow-Methods: GET, POST, PUT, OPTIONS`、
  `Allow-Headers: Content-Type`),nginx 原樣轉送;nginx 另加 `Cache-Control: no-store`。

| 方法 | 路徑(經 nginx) | 用途 / 回應 | 參數 | 會改到什麼 |
|---|---|---|---|---|
| GET | `/api/`(REST 的 `/`) | 狀態 `{"status":"ok"}` | — | 不改 |
| GET | `/api/settings/sensors` | 感測器 key/名稱對照 `[{"key":"s1","name":"inletWaterTemp"},...]` | — | 不改 |
| PUT | `/api/settings/sensors` | 改名稱,回 `{"ok":true}` | body JSON 陣列 `[{"key":"s1","name":"..."}]`;沒列出的 key 保留原值 | `settings.sqlite` 的 `sensor_config`(整表刪除後寫回合併結果);目前 app 其他地方**不讀**這張表 |
| GET | `/api/settings/frequency` | `{"read_frequency":1000}` | — | 不改 |
| PUT | `/api/settings/frequency` | 回 `{"ok":true,"read_frequency":n}` | body `{"read_frequency": n}`(n > 0) | `settings.sqlite` 的 `app_settings.read_frequency`;目前**沒有程式讀它**(Modbus 讀取週期不變) |
| GET | `/api/modbus/mode` | `{"mode":"network"}` | — | 不改 |
| PUT | `/api/modbus/mode` | 回 `{"ok":true,"mode":...}` | body `{"mode":"network"\|"standalone"}` | 只改 RESTManager 記憶體內的值並發 `modbusModeChanged`;Core **沒有接**這個 signal,不影響 Modbus;重啟後回 `network` |
| GET | `/api/sensor/range` | `sensor_data` 列,**分頁**:`{"page","pageSize","totalCount","totalPages","hasPreviousPage","hasNextPage","items":[{"ts":..,"s1":..,...,"s40":..}]}`(`items` 舊到新,可跨月) | `from`、`to`:epoch 秒(也接受 `123+60` / `123-60`);選填 `page`(預設 1)、`pageSize`(預設 200,最多 1000) | 不改 |
| GET | `/api/holding/range` | `holding_register` 列(`h1..h100`),分頁格式同上 | 同上 | 不改 |
| GET | `/api/device/sn` | `{"sn":"sn000000"}` | — | `device_info.ini` 不存在時建立 |
| GET | `/api/sensor/last` | 本月最新一列;沒有 → 404 `{"ok":false,"error":"no data"}` | — | 本月資料檔不存在時建立(SqlManager 開檔) |
| GET | `/api/holding/last` | 同上(holding) | — | 同上 |
| GET | `/api/sensor/rangeDateTime` | 同 `/api/sensor/range`(分頁) | `from`、`to`:ISO 8601(`2026-09-28T10:00:00` = 本機時間,結尾 `Z` = UTC)或 epoch 秒;選填 `page`、`pageSize` | 不改 |
| GET | `/api/sensor/rangeDateTimePage` | 同 `/api/sensor/rangeDateTime`(保留舊名稱) | 同上 | 不改 |
| GET | `/api/holding/rangeDateTime` | 同 `/api/holding/range`(分頁) | 同 `sensor/rangeDateTime` | 不改 |
| GET | `/api/holding/rangeDateTimePage` | 同 `/api/holding/rangeDateTime`(保留舊名稱) | 同上 | 不改 |

- 每個 `/api/...` 路徑都有 `OPTIONS`(CORS 預檢,200 + 上述標頭);`/api/settings/frequency` 的 OPTIONS 被註冊兩次(原程式),
  第一個生效,行為相同。錯誤:參數/JSON 不合法 400 `{"ok":false,"error":"..."}`、SQL 失敗 500、沒有的路徑 404。
- 目前 Manager 存檔時**不寫** `holding_register`,holding 類 range route 的 `items` 為空陣列、`/api/holding/last` 回 404 `no data`(原樣)。
- **區間查詢一律分頁**(w2-071):上表 6 個 range 路由回應格式相同。`page`、`pageSize` 要是正整數且不超過 2147483647,
  `pageSize` 大於 1000 時當成 1000(回應的 `pageSize` 是實際值);`from`、`to` 要在 `0` ~ `253402300799`
  (9999-12-31 23:59:59 UTC)之間且 `to >= from`,否則 400、不查詢。超過最後一頁 `items` 為空陣列;要整個區間請依
  `hasNextPage` 逐頁取。**與舊版不同**:以前 `range`、`rangeDateTime` 回傳整個區間的陣列 `[...]`(不分頁,大區間會讓 UI
  停住並佔大量記憶體),現在回傳分頁物件,原本讀陣列的程式要改讀 `items`。月份資料檔由資料夾列檔取得(不再逐月走訪),
  任何 `from`/`to` 都立即回應。RESTManager 仍在主執行緒以 blocking 方式等 SqlManager 執行緒,但每次最多讀 1000 列。
- 實測:w2-060 的 REST 檢查(simulator profile、經 `127.0.0.1` 與區網 IP 的 nginx 80、PUT 寫入後讀回再寫回原值、區網直連 18080
  被拒)在 `docs/evidence/w2-060/`;w2-062 起由 `scripts\verify-release-package.ps1` 在打包資料夾上重驗 `/api/` 狀態、
  `/api/settings/frequency`、CORS 與 OPTIONS 預檢,以及內部 port 在區網位址被拒。

### 區網連線(mirror,合併自 main e4bc327 / w2-042;w2-062 經 nginx)

| 端點 | 綁定(config.json,預設) | 用途 |
|---|---|---|
| nginx 網頁前端(另一個程式) | `0.0.0.0:<nginx.port>`(80) | 網頁 `http://<IP>/`、`/runtime.json`、**網頁同步 `/mirror`**(WebSocket 轉給 `127.0.0.1:<mirror.internalPort>`)、匯出檔 `/exports/<檔名>`(續傳)、REST `/api/...`(轉給 `127.0.0.1:<rest.port>`) |
| REST API(`RESTManager`,desktop app) | `rest` = `127.0.0.1:18080` | 內部 port,只限本機;區網經 nginx `http://<IP>/api/...`(見「REST API」) |
| 網頁 + CSV 下載(`AppHttpServer`,desktop app) | `http` = `0.0.0.0:8124` | 備援:`http://<IP>:8124/TaidaFlowApp.html`;匯出檔 `/exports/<檔名>`(不支援續傳) |
| mirror 區網轉發(`App/lanrelay.h`) | `mirror.publicBind:publicPort` = `0.0.0.0:8125` | 備援:nginx 沒開(`nginx.enabled=false`)時網頁連這裡;每條連線原樣雙向轉發到 `127.0.0.1:<mirror.internalPort>` |
| Mirror server(pack) | `127.0.0.1:<mirror.internalPort>`(18125) | 內部 port,只限本機,區網位址連不到 |

- 網頁版的 mirror host = 載入頁面的主機名稱(`location.hostname`),port = 同源 `/runtime.json` 的 `mirrorPublicPort`
  (程式依 config.json 寫:nginx 啟用時 = `nginx.port`,否則 = `mirror.publicPort`;讀不到時 8125);取不到主機名稱時退回
  `127.0.0.1` 並記 warning。所以從哪個位址開網頁,就連回同一個位址。
- Mirror `allowedOrigins = {}`(空清單 = 不限制 Origin);內網系統,依 Mango 決定不做存取控管,
  轉發也不做來源限制。
- 正式機 Windows 防火牆**只開 80(nginx)與 502(Modbus,不限來源)**(Mango 決定);8124 / 8125 / 18125 / 18080 不對外。
  `netsh` 範例見 `docs/DEPLOY_AND_STARTUP.md` §7。這由**管理員**設定,本專案的腳本不改防火牆 / 網路設定;
  首次啟動 Windows 可能跳出防火牆詢問視窗。
- **正式的網頁同步經 nginx 80 的 `/mirror`**(`nginx.enabled=true`,預設):正式機防火牆只開 80 與 502(Mango 決定),
  網頁一律連 `ws://<IP>/mirror`,由 nginx 轉給只綁本機的 Mirror(`127.0.0.1:<mirror.internalPort>`)。
- **LanRelay(8125,`App/lanrelay.h`)只是 nginx 未啟用時的備援**:`nginx.enabled=false`(或正式機的 `start-taidaflow`
  在 nginx 起不來時以 `nginx.enabled=false` 啟動 app)時,`runtime.json` 的 `mirrorPublicPort` 才是 8125,網頁改連它。
  pack 1.0.0/1.0.1 只允許 Mirror 綁 loopback,所以備援仍靠 LanRelay 轉發;**不再等待 pack 1.0.2**,它不是必要條件
  ——同步已經走 nginx。若日後 pack 提供 Mirror 綁非 loopback 的正式開關,可再評估是否移除 LanRelay。
- 8125 綁定失敗(例如被占用)時只記 `LAN relay could not listen on 0.0.0.0:8125: ...`,desktop 照常執行
  (本機的 Mirror 仍在 18125;經 nginx 的網頁仍可同步)。

### 離線提示(transport overlay,pack §9)

`TaidaFlowProxy` 有兩個 **`STORED false`**(不進 Mirror contract)的本機屬性 `transportReady`
(member 預設 `true`)與 `transportMessage`。只有 WASM 的 `main.cpp` 在載入 QML 前設成
`false / "Connecting to the Qt desktop Core..."` 並安裝 `transportStateHandler`。
`transportReady == false` 時:

- `TopNav.qml` 顯示紅色「離線」橫幅(含 transport 訊息);
- 所有會寫同步 property 的控制項 disabled:變頻器復歸、緊急停止、M1~M4 數值對話框、
  泵浦開關對話框、泵浦頻率對話框、二通閥對話框(已開的對話框會被關閉)、歷史頁上/下一頁
  (換頁是送給 desktop Core 的請求 `historyViewRequested`,見「歷史資料」);
- 畫面保留最後一份 authoritative 狀態;重連並收到完整 snapshot 後自動恢復。

desktop 上 `transportReady` 永遠是 `true`,外觀與行為不變(見驗證 4)。
純本機的檢視控制(分頁切換、警報頁分頁)不受影響;歷史頁的日期篩選 / 顯示前一周 / 下載 CSV / 取消
改由 desktop Core 執行(request signal 經 mirror),離線時由 HistoryPage.qml 停用。

### 網頁斷線偵測與自動恢復(心跳,w1-083 / w2-084)

起因:2026-10-01 現場網頁出現「半開」連線——網路斷了但瀏覽器沒收到 WebSocket 關閉,`transportReady` 仍是 `true`,
數值停住、按了沒反應、也沒有離線橫幅,按 F5 才恢復。現在分三層處理:

| 情況 | 網頁顯示 | 自動動作 |
|---|---|---|
| 5 秒沒收到桌面端心跳(連線看起來還在) | 橫幅「連線中斷,正在恢復…」+ 秒數倒數,所有操作停用,畫面保留最後狀態 | 心跳恢復立即解除 |
| 15 秒仍沒有心跳(連線仍看起來還在) | 橫幅副標「正在重新整理頁面…」 | 自動重新整理頁面(同 F5),受下方退避限制 |
| 網頁程式本身當掉(WebAssembly 中止 / 結束、未捕捉的 WebAssembly 執行錯誤)、載入失敗(下載失敗、瀏覽器不支援 WebAssembly) | 載入頁的紅字訊息 + 下一行「N 秒後自動重新整理頁面…」 | **10 秒**後自動重新整理,受下方退避限制 |
| 桌面真的關閉(WebSocket 正常關閉) | 原本的紅色「離線」橫幅 | 不重新整理,由套件自動重連 |

- **心跳(Core 端,w2-084)**:`Core/ServerHeartbeat.h/.cpp`。`Core::init()` 最後建立並啟動,先立即寫一次,之後在主執行緒以
  `QTimer` 每 `TaidaFlowProxy::kServerHeartbeatIntervalMs`(1 秒)把 `QDateTime::currentMSecsSinceEpoch()` 寫進
  `Td.serverHeartbeatMs`(鏡像到所有網頁,每秒一個很小的 patch)。不經過 SqlManager;刻意放在主事件迴圈:主執行緒卡住時心跳
  也停,網頁會如實顯示中斷。`Core::shutdown()` 第一步就停止心跳(關閉過程中不再寫)。只編進桌面版;網頁端只讀。
  log:`[Heartbeat] server heartbeat started: serverHeartbeatMs = epoch ms every 1000 ms (main thread)`,關閉時
  `[Heartbeat] server heartbeat stopped after <n> write(s)`。
- **偵測(網頁 QML,w1-083)**:`TaidaFlowContent/components/LinkWatchdog.qml/.js`,只在網頁執行;網頁不比較兩端時鐘,只用本機
  單調時鐘記錄「值最後一次變化的時間」;收到第一次變化才開始偵測;分頁被凍結 / 背景節流的時間不算沉默。
- **當機自動重新整理(載入頁,w2-084)**:`App/wasm/TaidaFlowApp.shell.html` 的 `taidaflow-auto-reload` 區塊。紅字訊息保留,
  倒數 10 秒(或退避要求的更久)後 `location.reload()`。只有 WebAssembly 的執行錯誤(`RuntimeError`、`Aborted(...)`)
  算當機;瀏覽器外掛等其他腳本錯誤不會重新整理。
- **退避規則(兩者共用)**:存在瀏覽器分頁的 `sessionStorage`(key `taidaflow.autoReload.lastEpochMs` 與
  `taidaflow.autoReload.streak`),重新整理後仍在。第一次自動重新整理不另等(心跳 15 秒 / 當機 10 秒後就做);之後每次與上一次
  至少間隔 60 秒、120 秒、240 秒,最多 5 分鐘;連線正常 60 秒後次數歸零。`sessionStorage` 被封鎖時,當機後改為 60 秒才重新整理。
- 改了 `TaidaFlowApp.shell.html` 要重新 configure 網頁版才會生效(`scripts\build-wasm.bat wasm-release fresh`;樣板是 configure
  相依,一般建置也會自動重跑 configure),再 `deploy-web.ps1` 部署。`serverHeartbeatMs` 是新的鏡像屬性,contract hash 因此改變:
  網頁檔與桌面必須是同一次建置(舊網頁快取連新桌面會被 contract mismatch 擋下)。
- 測試:`Core/tests` 的 `tst_server_heartbeat`(見「測試 / 驗證」5j)、`App\wasm\tests\run-shell-tests.bat`(5k,node,不開瀏覽器)、
  w1-083 的 Qt Quick Test(QtTester `qa/w1-083`)。

### 關閉流程(`Core::shutdown`,w2-067)

- desktop 正常關閉(按視窗的 X、`stop-taidaflow` / `verify-desktop-startup.ps1` 送的 WM_CLOSE、`taskkill` 不加 `/F`)時,
  `QCoreApplication::aboutToQuit`(`Core::init()` 最先連接)呼叫 `Core::shutdown("aboutToQuit")`,在 application 物件還在時
  依序停止並釋放後端:
  0. (w2-084)停止伺服器心跳 `serverHeartbeatMs`(關閉過程中不再寫);
  1. 中斷 SqlManager → Core 的歷史結果連線;
  2. Manager 停止(輪詢計時器、MS300、5 台 ADAM 的 Modbus TCP 連線)→ Modbus 伺服器停止 → 兩者刪除;
  3. REST API → 歷史檢視 → CSV 匯出(join 匯出執行緒)→ `AppHttpServer`(join 它的執行緒);
  4. 最後 SqlManager(`SqlManager::shutdown()`:在 worker 執行緒關閉 SQLite 連線後 join;之後的同步請求立即回失敗並記 warning)。
- log:`[Core] shutdown (aboutToQuit): stopping the backend` → 各服務的 stopped 行 → `[Core] shutdown complete in <n> ms`。
  只執行一次(可重入)。`main()` 在 `app.exec()` 之前就返回時(QML 載入失敗等),由 `qAddPostRoutine` 在 `QApplication`
  解構時執行同一個 shutdown;`~Core` 只刪 Proxy。
- 修正前,連著設備(或模擬器)關閉會以 0xC0000005 結束(`~Core` 在 C runtime 結束階段才停 Manager,用到已解構的 static);
  修正後接模擬器 10/10、不接設備 3/3 皆 exit 0(證據 `docs/evidence/w2-067/`)。QTest:`docs\evidence\w2-067\tools\run-qtest.bat`
  (`tst_w2067_sqlmanager_shutdown`,見「測試 / 驗證」5i)。

## 正式機部署(w2-057 / w2-062)

操作人員版(打包內容、第一次部署、日常操作、更新、手動部署、config.json 欄位表、防火牆、常見問題、已決定事項)在
**[docs/DEPLOY_AND_STARTUP.md](docs/DEPLOY_AND_STARTUP.md)**;打包時複製成打包資料夾的 `DEPLOY.md`。這裡只列開發者要知道的部分。

- **打包**(開發機):`powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-release.ps1 [-NginxDir <nginx 資料夾>] [-Force]`
  → `dist\TaidaFlow-<yyyyMMdd>-<git 短雜湊>[-dirty]\`(`dist/` 已加入 `.gitignore`)。`build\desktop` / `build\wasm-release`
  以 `ninja -n` 確認是最新,否則 exit 3(`-AllowStale` 可略過,不建議)。其他參數:`-OutDir`(預設 `dist`)、`-BuildDir`、`-WebSource`、`-QtDir`。
  附上的 nginx:`-NginxDir`,沒給時用開發設定 config.json(`TAIDAFLOW_CONFIG`,否則 `deploy\dev\config.dev.json`)的 `nginx.exe`
  所在資料夾,再不行才找最新的 `C:\tools\nginx\nginx-<版本>`(w2-065)。內容:`TaidaFlowApp.exe`;`windeployqt`(Qt 6.8.3,`--release --no-compiler-runtime
  --no-translations --skip-plugin-types qmltooling,canbus --exclude-plugins qsqlmimer,qsqlodbc,qsqlpsql`,`--qmldir`
  指向 `TaidaFlowContent`、`TaidaFlow`、`Dependencies`)帶入的 Qt DLL / plugins / `qml\`;**MSVC 執行環境採 app-local**
  (`Microsoft.VC145.CRT` 的 DLL,免安裝 vc_redist);`web\`(同 `deploy-web.ps1 -NoConfig`,含 `.gz` 與預設的 `runtime.json`);
  `deploy\release\` 的正式機腳本(w2-065 起沒有 `logging\quiet.ini`)、`scripts\install-nginx-config.ps1` + `scripts\taidaflow-config.ps1`
  + `deploy\nginx\taidaflow.conf`、**`nginx\`**(nginx for Windows 1.30.5:`nginx.exe`、`docs\` 授權檔、`conf\` 但**不含**
  `nginx.conf`、`SOURCE.txt`)、`DEPLOY.md`、`VERSION.txt`、`MANIFEST.txt`(每檔大小與 SHA-256)。
  **不含** `config.json`(正式機第一次啟動時建立,更新不會覆蓋)、`nginx.conf`、資料庫、ini、log、`.py`、`__pycache__`、`.lib/.pdb`
  (打包最後檢查,出現即 exit 6);文字檔與 `TaidaFlowApp.exe` 裡不能有建置機的路徑(repo、使用者資料夾;出現即 exit 6)。
- **DLL 相依檢查**:`scripts\check-package-deps.ps1 -Package <資料夾>`(`dumpbin /dependents`,每個 exe/dll 的匯入都要在打包資料夾或
  System32 找到;Qt 安裝目錄不算)。
- **正式機腳本**(`deploy\release\`,在打包資料夾最上層),全部讀 `<安裝資料夾>\config.json`(沒有時以
  `TaidaFlowApp.exe --write-default-config` 建立,腳本不重複定義預設值):
  - `start-taidaflow.ps1/.bat`(**不做開發機安全探測**):單一執行個體與 config.json 各 port 占用檢查(列出占用者,含 pid 4 = HTTP.sys,
    不關別人的程式);一律移除 `TAIDAFLOW_DEVICE_PROFILE`;PATH 去掉含 `Qt6Core.dll` 的資料夾;`TAIDAFLOW_CONFIG` = 這份
    config.json;nginx(`nginx.enabled`):以 `install-nginx-config.ps1 -IfChanged` 確認 `nginx\conf\nginx.conf` 是這個安裝資料夾
    / 這份 config.json 產生的(不是就重產並對執行中的 nginx `-s reload`),沒在跑就以 `start nginx` 的方式啟動,已在跑就不動;
    nginx 不能用時 app 以 `nginx.enabled=false` 啟動(exit 8);確認 `http.port`、`mirror.publicPort` 在聽。
    `-DataDir`、`-LogDir`、`-UseNginx` / `-NoNginx`、`-Port`、`-RestPort`、`-Nginx` 是**單次覆寫**(寫成 `<資料資料夾>\config.effective.json`
    交給 app,config.json 本身不改);另有 `-Config`(另一份 config.json)與 `-StartTimeoutSec`(等 port 開始聽的秒數,預設 60)。w2-065:不再轉存程式輸出、不設 `QT_LOGGING_CONF`(`-AppLog`、`-KeepLogDays` 移除);
    app 以沒有主控台視窗的方式啟動;nginx 位置 = config.json `nginx.exe`(相對於 config.json 所在資料夾);
    `launcher-YYYY-MM-DD.log` 寫在 `log.dir`,啟動時清理過期的 launcher / nginx access log。
  - `stop-taidaflow.ps1/.bat`:以 WM_CLOSE 正常關 app(app 走「關閉流程」的 `Core::shutdown`,接設備時也 exit 0;
    最多等 `-TimeoutSec` 60 秒,逾時只報告(exit 7),`-Force` 才強制);**不停 nginx**(要停時在 nginx 資料夾 `nginx -s quit`)。
    只關 `<資料資料夾>\taidaflow-app.json` 記錄的那個 app(啟動時用了單次 `-DataDir` 才需要給同樣的 `-DataDir`;
    log 資料夾由 `taidaflow-app.json` 得知,`-LogDir` 通常不用給)。
  - `register-autostart.ps1` / `unregister-autostart.ps1`(工作排程器「使用者登入時」,排程只帶 start-taidaflow.ps1,設定都讀
    config.json;**先 `-WhatIf`**,本專案只做過乾跑)。
  - **w2-076 現場標準做法**(DEPLOY §2A):`scripts\install-nginx-service.ps1` / `uninstall-nginx-service.ps1`(系統管理員;`-WhatIf` 不需要)
    以包內 WinSW 2.12.0(`nginx\nginx-service.exe`,來源與 SHA-256 在 `nginx\SOURCE-WinSW.txt`,取得方式 BUILD.md §2.8)把 nginx 註冊成
    Windows 服務 `TaidaFlowNginx`(`nginx -p <nginx 資料夾>`、停止 `nginx -s quit`、Automatic,XML `nginx\nginx-service.xml` 在現場產生);
    `scripts\add-startup-shortcut.ps1` / `remove-startup-shortcut.ps1` 在開機啟動資料夾建立 / 移除捷徑「TaidaFlow」(與工作排程器擇一)。
    `start-taidaflow.ps1` 以 `Win32_Service`(PathName = 本安裝的 `nginx-service.exe`)+ 行程父子關係認出服務的 nginx:port 80 視為正常、
    不再 `start nginx`、服務停止時改啟動服務、`nginx.conf` 重產時非系統管理員不 reload(結果碼 3)。判斷函式在 `scripts\taidaflow-config.ps1`,
    單元測試 `docs\evidence\w2-076\tools\test-nginx-service-logic.ps1`。**服務的安裝 / 開機啟動只在開發機做了乾跑與非管理員拒絕的驗證,
    實際安裝要在現場(或測試機)以系統管理員實測。**
  - 舊的站台批次檔(以 `.bat` 設定資料夾與 nginx)機制已刪除,由 config.json 取代。
- **開發機上驗證打包資料夾**:`powershell -ExecutionPolicy Bypass -File scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<...>
  [-Mode ps1|bat] [-SeedDb <sensor_yyyyMM.sqlite>]`:打包內容(無 config.json、附 nginx、無 nginx.conf、無建置機路徑)→ 安全探測 SAFE →
  以不含 Qt 的 PATH:A 第一次啟動(建立 config.json 與資料資料夾;`C:\TaidaFlowData` 以單次 `-DataDir` 換成 `build\` 下的測試資料夾)→
  B 改 config.json 的 dataDir、`install-nginx-config.ps1`、啟動(nginx `start nginx` + app)、curl 檢查網頁 / `runtime.json` no-store /
  `/mirror` 101 / REST / 匯出下載與 Range、DLL 來源、log、停止(nginx 保持執行,再 `nginx -s quit`)→ C 整個資料夾搬到另一處,啟動時
  自動重產 nginx.conf → D 壞掉的 config.json:腳本與程式都 exit 2 且不覆寫 → E 還原成出貨狀態。不操作畫面。
  w2-065 另檢查:`<資料資料夾>\logs` 有當天的 quiet / full / launcher 檔(full 有 info、quiet 只有 warning 以上)、
  nginx access log 以日期命名、壞 JSON 時備援 log 在打包資料夾的 `logs\`。
- **開發機腳本與正式機腳本不可混用**:開發機一律用 `scripts\run-desktop.ps1`(安全探測);正式機腳本沒有保護。

## 接 Adam60xxSimulator(測試用設備位址切換)

`Adam60xxSimulator`(repo 內另一個專案,**只執行它已建好的 exe**,不建置、不在其資料夾寫檔)
以 `--autostart` 在 `127.0.0.201~205:502`(Unit ID 1)開五台 ADAM。接模擬器有兩種方式:

| 做法 | 五台 ADAM 位址 | log |
|---|---|---|
| `deploy\dev\config.dev.json`(廠區位址)+ `TAIDAFLOW_DEVICE_PROFILE=simulator`(`run-desktop.ps1 -DeviceProfile simulator`) | 各台換成 `127.0.0.201~205`(port、unit 用 config.json 的) | `[Modbus] device profile=simulator`,每台一行,註明 `configured host 192.168.1.20x replaced by TAIDAFLOW_DEVICE_PROFILE=simulator` |
| `deploy\dev\config.simulator.json`(位址直接是 `127.0.0.201~205`,`run-desktop.ps1 -Config deploy\dev\config.simulator.json`) | `127.0.0.201~205` | `[Modbus] device profile=default` + 五行位址 |
| `TAIDAFLOW_DEVICE_PROFILE` 其他值 | 維持 config.json 的位址 | 先記 `[Modbus] Unknown TAIDAFLOW_DEVICE_PROFILE="…"` 警告 |

只換位址:重連、補送、interlock、MS300、聚合 Modbus server 行為都不變。正式機的 `start-taidaflow` 一律清除 `TAIDAFLOW_DEVICE_PROFILE`。

```powershell
# 1. 先起模擬器(工作目錄 build\sim-cwd;502 已被占用就拒絕,不會關掉別人的程式)
powershell -ExecutionPolicy Bypass -File scripts\run-simulator.ps1
# 2. 再起桌面版(simulator 模式探測 + 幫 app 設 TAIDAFLOW_DEVICE_PROFILE=simulator)
powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -DeviceProfile simulator -Label "sim"
# 3. 網頁版:有 nginx 時 http://127.0.0.1/ ,沒有時 http://127.0.0.1:8124/TaidaFlowApp.html
```

- **順序一定是模擬器先**。實測(`docs/evidence/wasm-v4-sim/14-bind-order-core-first.txt`):
  app 先起時,它對 `127.0.0.20x:502` 的 ADAM 連線會被 app **自己的** `0.0.0.0:502` 聚合 server
  接走(讀值全 0、6224/6256 回 Modbus exception),之後再起模擬器也不會換過去。所以
  安全探測在實際生效的設備位址是本機位址時,要求那些端點都已由 `Adam60xxSimulator.exe` 在聽,否則 exit 5
  (`SIMULATOR-NOT-READY`),`run-desktop.ps1` 不啟動。模擬器先起時,app 的 `0.0.0.0:502`
  仍 bind 成功,兩者並存(連 `127.0.0.20x` 的連線由較精確的模擬器 bind 接手)。
- simulator profile 的探測:照舊 TCP 探測 config.json 的廠區位址(可達 → exit 3)、列序列埠(設定的 MS300 序列埠存在 → exit 3);
  `modbusServer.port` 的 listener **只**接受「程序映像檔是 `Adam60xxSimulator.exe` 且位址是實際生效的設備本機位址」,
  其他(別的程式、別的位址、IPv6)一律 UNSAFE(exit 3)。紀錄預設寫到 `build\runtime-logs\safety-probe-sim.log`。
- 已知點位落差(不是切換造成,之後另案處理):模擬器 ADAM-6224 DI0 是「漏液」且預設 0,
  Core 把 DI0 當「相位正常」→ 啟動後立即 `[Safety Interlock] ... DI0 is false`,泵浦啟動會被擋;
  Core 補水泵寫 6256 coil 19,模擬器製程只把 coil 16 當泵浦。
- 模擬器只在值**改變**時記 `Core → ADAM-…` log(`QModbusServer::dataWritten`);寫入同值不會出現,
  以 Core log 的 `[Modbus][Write completed]` 為準。
- 探測自我測試(真的起模擬器與假 listener,exit code 判定;default / simulator / simcfg 三種設定):
  `powershell -ExecutionPolicy Bypass -File scripts\probe_selftest_sim.ps1`。
- 注意:`run-desktop.ps1` / `run-simulator.ps1` 的輸出若接到管線(`| Select-Object` 等),被啟動
  的程式會繼承該管線,指令要等程式結束才返回。**在 PowerShell 裡用 `>` 導向檔案也一樣**(PowerShell 以管線收集
  外部程式的輸出);直接執行,或在 **cmd** 以 `>` 導向檔案即可(w2-069 以同樣的 `Start-Process` 啟動方式實測,
  `docs/evidence/w2-069/03-pipe-redirect-test.txt`)。

## 介面字型(桌面與網頁:內嵌 Noto Sans TC)

**桌面版與網頁版都使用內嵌的 Noto Sans TC(思源黑體)子集作為介面字型**(字型名稱 `TaidaFlow Noto Sans TC`),
會跳動或需要對齊的數值、日期時間與數值輸入框保留 **Consolas**(w1-081 的清單),Consolas 數值中的中文同樣用 Noto Sans TC(w2-082)。
Qt for WebAssembly 沒有系統字型,桌面版則原本會混用微軟正黑體、Segoe UI 與新細明體,所以兩邊統一用同一份內嵌字型(OFL-1.1):

- `App/fonts/TaidaFlowNotoSansTC-{Regular,Bold}.ttf` 與 `App/fonts/charset.txt` **已在 git 裡**,建置直接使用
  (CMake 不產生、不呼叫外部程式);clone 下來、部署都**不需要 Python**。
- 加了新的中文字串、網頁出現方框時才要重新產生:這是專案唯一的 Python 工具(`scripts\make_font_subset.py`,用 fonttools;
  以 `scripts\make-font-subset.ps1` 執行,`--check` 只檢查)。刪掉中文字串或註解、有字不再使用時 `--check` 也是 exit 1
  (訊息 `0 new chars`,不缺字),同樣重新產生即可(w2-067-fix1)。需求、步驟與提交方式見 [docs/BUILD.md §2.5](docs/BUILD.md)。
  它掃描 `App/ Core/ TaidaFlow/ TaidaFlowContent/ Dependencies/` 的 QML/JS/C++ 非 ASCII 字元 + 可列印 ASCII + 常用全形標點
  + Latin-1 補充(U+00A0–U+00FF)、全形數字與標點、常用單位 / 箭頭 / 比較 / 引號符號(w2-082,因為它是介面字型,英文、數字與符號
  不能逐字換成別的字型),實體化 wght 400/700 並子集化。
- 來源字型:`C:\Windows\Fonts\NotoSansTC-VF.ttf`(Noto Sans TC 2.004,Windows 11 內附;亦可從
  <https://fonts.google.com/noto/specimen/Noto+Sans+TC> 下載 `NotoSansTC[wght].ttf` 以 `--source` 指定)。
  授權 SIL Open Font License 1.1,宣告保留於字型 name table。
- 載入與套用:`App/embeddedfonts.cpp` 在兩平台都 `addApplicationFont`;`App/main.cpp` 的 HOOK 那一行把它交給 `applyUiFont()`
  (應用程式字型、`App.qml` 的 ApplicationWindow `font.family: Application.font.family`、`Consolas` 的替代字型;
  載入失敗時退回微軟正黑體 UI),log 一行 `[UiFont] interface font: TaidaFlow Noto Sans TC | Consolas -> taidaflow noto sans tc`。
  **只有 WASM** 另外先建 fallback 鏈(`QFont::insertSubstitutions` + Han/Common script fallback):其他字型名稱都退回 Noto Sans TC,
  `Consolas` 先用 Qt 網頁版內附的等寬 DejaVu Sans Mono 顯示數字、中文再退回 Noto Sans TC。
- 測試:`App\tests` 的 `tst_uifont` / `tst_uifont_wasmpath`(直接編譯 `main.cpp` 的字型程式碼,桌面 / WebAssembly 兩種順序;
  應用程式字型、QFontInfo、Consolas 替代清單、實際字形來源、子集涵蓋、ApplicationWindow 與 Controls 的字型),見 `App/tests/README.md`。

## 開發用工具(dev-only)

- w2-062 起專案的檢查 / 執行 / 打包工具都是 PowerShell 或批次檔:`check-wasm-backend.ps1`、`check-version-shadow.ps1`、
  `check-rest-routes.ps1`、`verify-pack.ps1` 取代原本同名的舊腳本(輸出逐行相同,證據 `docs/evidence/w2-062/05-*`);
  `verify-desktop-startup.ps1` 以 `Process.CloseMainWindow`(WM_CLOSE)正常關閉 app(原本的輸入工具已刪除)。
  唯一的 Python 是字型子集(上一節)。`docs/evidence/` 下的歷史證據與舊工具不變,但不再列為要跑的步驟。
- w2-060 已移除測試專用的 PV 注入;無設備時改用 Adam60xxSimulator(見「接 Adam60xxSimulator」)。
- w2-059 已移除 UI 截圖比對與舊 CSV 驗證工具:不再做 UI 自動化測試,歷史頁 / 匯出改由 5b / 5e 的 QTest 驗證。
- 舊的開發用網頁伺服器(8123)已隨 w2-049 移除,網頁改由 desktop 的 `AppHttpServer` 與 nginx 提供。

## 歷史資料:時間區間與匯出(w2-041,`docs/taidaflow_history_export_spec.md`)

### 時間區間查詢:各連線端獨立的檢視(spec §2 / §2.1,w2-052)

- 每個連線端(桌面 `desktop`、每個網頁分頁 `web-xxxx`,即 `Td.clientSessionId`)有**自己的區間與頁碼**;
  一端篩選或翻頁,其他端的畫面不變(2026-09-27 Mango 問題 8,取代原本全部連線端共用一組區間與頁碼的做法)。
- 唯一的請求:`Td.historyViewRequested(sessionId, fromMs, toMs, page)`(WASM 經 mirror relay 到 desktop)。
  進入歷史頁(第一次 = 當月第 1 頁)、「篩選」/ Enter、「顯示前一周」(今天往前 7 天,含今天)送 page 1,
  上/下一頁送目前頁 ∓ 1;`fromMs/toMs` 永遠是該端自己的區間,Core 只照請求分頁、不保留會影響請求的區間。
  單位 epoch 毫秒、本機時區、兩端都含(w2-040 契約);`0 .. 8640000000000000` = 不限區間
  (UI 已不送,Core 仍接受)。
- 處理者:`Core/HistoryViews.{h,cpp}` 的 `HistoryViewService`(`Core::init` 建立)。驗證:sessionId 必須是
  `[A-Za-z0-9_-]{1,40}`(與匯出相同)、區間為有限值且 from <= to、page >= 1;不合法的請求只記 warning,
  不寫任何東西、也不查詢。
- 結果寫到同步屬性 `Td.historyViews[sessionId] = {fromMs, toMs, page, totalPages, totalRows, records, revision}`:
  `records` = 本頁 10 筆(新到舊),每筆 `{timestampMs, values}`,`values` 依 `Td.historyTitle` 欄位順序一欄一格
  (時間字串 `yyyy/MM/dd HH:mm:ss` + 16 個換算後數值,無值為 null);`page` 超過總頁數時夾到最後一頁
  (Core 自動改要最後一頁);`totalPages` 至少 1。每次都以 `setHistoryViews` 寫**整個 map**;`revision`
  取自 Core 全域遞增計數,只有該 entry 內容變了才換(同一請求結果相同就不寫)。各端 QML 只讀自己的 key,
  revision 變了才重畫。`historyTitle` 仍是所有連線端共用,啟動時設一次。
- 生命週期:網頁端超過 30 分鐘沒有請求,移除其 entry 與 SqlManager 內該端的狀態(每分鐘檢查一次,
  實際為 30~31 分鐘);最多同時 32 個 entry(含 `desktop`),新的一端進來時先移除最久未用的網頁端;
  `desktop` 永不移除;被移除的一端下次請求會重建(新的 revision)。移除時同樣只寫一次整個 map。
  Core 啟動時不預先載入任何一端,也不因每秒存檔而重載。
- 查詢在 SqlManager 執行緒非同步分步執行(w2-039/w2-045:每步約 6 ms、步驟之間每秒存檔可插隊、月份檔快照、
  COUNT 快取、相鄰頁用 keyset 錨點);**過時判定與 keyset 錨點依 sessionId 分開**
  (`SqlManager::requestSensorHistoryRangePage(sessionKey, ...)`、`releaseHistorySession`):只有同一端較新的請求
  會讓自己的舊請求作廢,A 端不會讓 B 端的請求作廢或結果被丟,也不會搶走 B 端的錨點。
- 秒換算:`ceil(fromMs/1000) .. floor(toMs/1000)`,且至少從 1 開始(timestamp <= 0 的列歷史頁本來就不顯示)。
- 跨月:`SqlManager::requestSensorHistoryRangePage` 只列出資料夾裡**實際存在**且與區間相交的
  `sensor_YYYYMM.sqlite`(不逐月走,不限區間也不會掃幾百萬個月),新月份在前;總數 = 各月 COUNT 加總;
  頁面在月份間串接(每月內 `ORDER BY timestamp DESC, rowid DESC`)。各月 COUNT 有快取,以(月份檔, 區間)為 key
  (w2-052 起,各端不同區間不會互相擠掉;最多 256 筆,超過丟最久未用的):月份檔大小與 SQLite 檔頭
  change counter 都沒變才直接沿用,只新增了列時只數新增的列。

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
- 狀態表上限(w2-071):`sessionId` 不合法的要求**不存入** `historyExportStatus`(只記 log,最多每 60 秒一行並附期間
  另外拒絕的次數);合法 sessionId 被拒(例如區間錯誤;已有匯出進行中時仍照上面的規則只改原本那筆的 `message`)存成 `error`
  項目,與 done / cancelled 一起算在「已結束最多 30 筆」內(最舊的先移除;排隊中/執行中的項目不會被移除);網頁匯出最多 20 筆排隊,
  再要求 → `error`「匯出排隊已達上限,請晚點再試」。所以狀態表最多 30 + 1(執行中)+ 20(排隊)+ 1(desktop)筆。
  欄位與 `state` 值不變。
- 生成(`HistoryExportWorker`,專用低優先權執行緒):用**自己的唯讀 SQLite 連線**讀月份檔(新月份在前),
  以 keyset 分段(每段 <= 2000 列,每段是一個獨立的短 statement,讀鎖只持有一段),邊讀邊寫
  (1 MB 緩衝)到 `QSaveFile`,完成才改名成正式檔名;取消 / 失敗時暫存檔丟棄。記憶體不隨檔案大小成長。
  SqlManager 執行緒(每秒存檔、歷史頁)不被匯出占用。
- CSV:UTF-8 BOM、CRLF、每格加雙引號;欄位 = `序號` + 歷史頁的 17 個標題(`時間`、TT-01..04、
  PT-01..07、FM-01、M1..M4,與 `Td.historyTitle` 同一份定義);順序同歷史頁(新到舊),序號 1..N;
  數值 = 歷史頁的換算(TT/M ×100/65535、PT ×1000/65535、FM-01 ×1),格式與頁面 `toFixed(2)` 相同
  (含剛好 .xx5 的進位規則);空值 `—`。檔名 `<sessionId>_<yyyyMMdd_HHmmss>.csv`(要求時間)。
- **Offset**(w2-086):資料庫存的是**存檔當時已加上設定頁 offset** 的值,所以歷史頁、CSV、REST 都是校正後的值
  (與主畫面一致,差不超過半個計數);改 offset 只影響之後存的資料,舊資料不改寫。見「存檔時套用 offset」。
- **桌面**(`desktop`):先跳「下載歷史資料」另存新檔對話框(預設「文件」資料夾 + 上述檔名),選定後
  同樣排隊/進度/可取消,直接寫到選定位置(不經暫存、不占名額),完成填 `savedPath`;對話框按取消 →
  `state = cancelled`(「已取消儲存」)。
- **網頁**:寫到 **`<desktop 工作目錄>\exports\`**(= config.json 的 `dataDir\exports`;開發機 `build\runtime-cwd\exports\`;與 `data\`、
  `settings.sqlite` 同一個基準,重開 app 後連結仍有效)。完成時 `url = "/exports/<檔名>"`、
  `downloadPort` = config.json 的 `nginx.enabled ? nginx.port : http.port`(預設 80 = nginx;`TAIDAFLOW_DOWNLOAD_PORT` 可臨時覆寫,
  見「nginx 網頁前端與下載」)。Core 不知道網頁用哪個主機名稱,所以只送路徑 + port;下載連結由網頁組成
  (`HistoryPage.qml` 的 `exportDownloadUrl()`):`url` 以 `/` 開頭 → `"http://" + Td.pageHost + ":" +
  downloadPort + url`(downloadPort 缺少時用 8124),例如 `http://192.168.0.125:8124/exports/<檔名>`;
  已是 `http...` 的完整網址則原樣使用。`Td.pageHost` 是 Proxy 的 **`STORED false`** 本機屬性(不進 Mirror
  contract),WASM `main.cpp` 設成與 mirror host 相同的 `location.hostname`(同一個 `127.0.0.1` 後備);
  desktop 保持空字串。「下載檔案」按鈕與完成時自動開啟都用這個網址。
  清理:每產生一個網頁檔後,若 `*.csv` 超過 20 個**或**總量超過 2 GB,依修改時間刪最舊的,直到兩個條件都
  滿足;不刪剛產生的檔,它單獨就超過 2 GB 時保留並記 log;正在被下載而刪不掉的檔記 log 跳過。
  啟動時刪除上次當機留下的暫存檔(`*.csv.XXXXXX`)。
- **下載**(w2-049 起為 `AppHttpServer` 單例的 `/exports` 掛載:`HistoryExportManager` 建立時掛上、
  shutdown 時卸下;listener = config.json `http`(預設 `0.0.0.0:8124`)由 Core 啟停):只有 `GET`/`HEAD /exports/<檔名>`;檔名必須是
  `<sessionId>_<yyyyMMdd_HHmmss>.csv`(不能含 `/ \ .. :`、不能是其他副檔名),且實際路徑必須在匯出
  資料夾內(符號連結不跟隨);回應 `Content-Type: text/csv; charset=utf-8`、
  `Content-Disposition: attachment; filename="<檔名>"`、`Access-Control-Allow-Origin: *`、
  `Cache-Control: no-store`、`Content-Length`;檔案以 `QHttpServerResponder::write(QIODevice*)` 從磁碟
  分段送出(不整檔讀進記憶體)。不存在 404、壞檔名 400、其他方法 405。
  綁定失敗只記 warning,app 繼續執行(網頁匯出檔仍會寫出)。
  用 nginx 時(`nginx.enabled`,預設)下載由 nginx 直接從匯出資料夾送(支援 Range 續傳),
  8124 的掛載保留作備援(不支援 Range)。
- 舊的 `Td.saveHistoryCsv()`(Q_INVOKABLE,前端組 10 筆 CSV)**已刪除**(main w2-042 自
  `TaidaFlowProxy.h` 移除,已合併進本分支);CSV 一律走上述匯出佇列。`docs/wasm-integration-report.md`
  中關於它的段落是歷史紀錄。

## 警報頁:各連線端獨立的時間區間與分頁(w2-080)

介面契約在 `Core/TaidaFlowProxy.h` 的 `alarmViews` 區塊(main w1-078,Mango 核准);做法與歷史頁的 `historyViews` 相同。

- 每個連線端(桌面 `desktop`、每個網頁分頁 `web-xxxx`,即 `Td.clientSessionId`)有**自己的區間與頁碼**;
  一端篩選或翻頁,其他端的畫面不變。
- 唯一的請求:`Td.alarmViewRequested(sessionId, fromMs, toMs, page)`(WASM 經 mirror relay 到 desktop)。
  警報頁顯示時、「篩選」/「顯示前一周」(page 1)、上/下一頁,以及預設的「最近 24 小時」在 `alarmRecords` 變動時
  (新的最近 24 小時、page 1)送出。`fromMs/toMs` 為本機時區 epoch 毫秒、兩端都含
  (起始分鐘 :00.000 .. 結束分鐘 :59.999),可跨月份。
- 處理者:`Core/AlarmViews.{h,cpp}` 的 `AlarmViewService`(`Core::init` 建立)。查詢在 SqlManager 執行緒非同步分步執行
  (`SqlManager::requestAlarmRangePage`,`Core/SqlManagerAlarms.cpp`):只列出資料夾裡實際存在且與區間相交的
  `sensor_YYYYMM.sqlite`(不逐月走),新月份在前,每步最多讀一個月份檔的 2000 筆,步驟之間每秒存檔可插隊;
  過時判定依 sessionId 分開,只有同一端較新的請求會讓自己的舊請求作廢,A 端不會讓 B 端的請求作廢。
- 結果寫到同步屬性 `Td.alarmViews[sessionId] = {fromMs, toMs, page, pageSize, totalCount, totalPages, activeCount,
  rows, state, message, revision}`:
  - 排序新到舊(`occurrence_time` 由新到舊,同一秒依 id 由大到小,與 `alarmRecords` 相同);每頁 9 筆(`pageSize` 9);
  - `totalCount` = 整個區間的警報數;`totalPages` 至少 1(沒有警報時為 1);`activeCount` = 區間內 `alarmStatus`
    為「未處理」的筆數;
  - `rows` 每筆的欄位與格式和 `alarmRecords` 完全相同(`id`、`timestampMs`、`alarmTime`、`equipment`、`sensorName`、
    `alarmMessage`、`severity`、`alarmStatus`),另加 `serialNumber` = 在整個區間中的位置(從 1 起算)。兩者用同一個轉換函式
    (`Core/AlarmRecordFormat.{h,cpp}`,由 `core.cpp` 原樣搬出,`Core::loadAlarmRecords` 也改呼叫它,`alarmRecords`
    本身與「最近 3 天」的載入不變);
  - `page` 超過最後一頁時夾到最後一頁、小於 1 時為 1;
  - `state` 為 `ready`;區間讀不到時為 `error`,`message` 是頁面顯示的錯誤文字。
  - 每次都以 `setAlarmViews` 寫**整個 map**;`revision` 取自服務的遞增計數,只有該 entry 內容變了才換
    (同一請求結果相同就不寫)。各端 QML 只讀自己的 key,revision 變了才重畫。
- 即時更新:警報寫入(`SqlManager::insertAlarm`)或解除(`SqlManager::updateAlarmReason`)成功後,Core 重新查詢
  所有「區間涵蓋這筆警報時間」的 entry,保留該端的區間與目前頁(頁數夾到新的總頁數);區間外的 entry 不動、revision 不變。
- 不合法的請求:sessionId 不是 `[A-Za-z0-9_-]{1,40}` → 不寫任何東西,warning 每分鐘最多一行(附略過的次數);
  sessionId 合法但區間不是有限數值、from > to、或超出 `0 .. 253402300799999`(9999-12-31 結束)→ 該端 entry 寫
  `state: "error"` 與 `message`(不查詢)。查詢失敗同樣寫 `state: "error"`。
- 生命週期(與 `historyViews` 相同):網頁端超過 30 分鐘沒有請求就移除其 entry(每分鐘檢查一次);最多同時 32 個 entry
  (含 `desktop`),新的一端進來時先移除最久未用的網頁端;`desktop` 永不移除;被移除的一端下次請求會重建。
  關閉時在 `Core::shutdown` 的第 2 步(SqlManager 關閉之前)停止。
- 測試:`Core\tests\run-core-tests.bat` 的 `tst_alarm_views`(見「測試 / 驗證」5j)。不經瀏覽器的連線測試工具:
  `docs\evidence\w2-080\tools\build-mirror-alarm-client.bat`(Proxy Mirror 用戶端,送 `alarmViewRequested` 並印出
  `alarmViews[sessionId]`;由 w2-050 的匯出用戶端改寫)。

## 設定頁上下限超限警報(w2-085)

設定頁每支感測器的上限 / 下限(`Td.sensorSettingsSv`,契約與完整規則見 `Core/SensorSettings.md`「後端超限警報規則」)
由桌面後端判斷並寫進警報紀錄,做法與漏水感測器(DI1)相同:超限時**立即新增**一筆,回到範圍內**持續 2 秒**後把**同一筆**
改為「已解除」。

- 程式:`Core/LimitAlarms.{h,cpp}`(`LimitAlarmMonitor`,新檔),`Manager` 只做接線:建構時建立、`updateProcessPoint`
  寫完 PV 後通知、`alarmSaved` 轉給 `Core::loadAlarmRecords`;警報頁各端檢視(`alarmViews`)由 SqlManager 的
  `alarmHistoryChanged` 自動更新。只編進桌面版(WASM 不含)。
- 判斷:校正後數值(原始 PV + offset;Filter 壓差 = 校正後 PT-02 − 校正後 PT-03)> 啟用的上限、或 < 啟用的下限;
  等於上下限屬正常;壓力一律 kPa(與顯示單位無關)。PT-04/PT-05 等同組感測器各自判斷、各自記錄;上限與下限各自一筆。
- 紀錄:`sensorName` 用畫面名稱(`PT-04`、`TT-01`、`流量計`、`Filter 壓差`),訊息例如
  `超過上限：612.35 kPa（上限 600.00 kPa）`、`低於下限：18.20 °C（下限 20.00 °C）`;嚴重程度一律「警告」。
  未解除前同一感測器同一方向不重複新增;回到範圍又在 2 秒內再超限 → 不解除、不新增。
- 設定改變(套用、停用)時立即以新設定重新判斷;停用視為回到範圍(2 秒後解除)。
- 重啟:比照 w2-053 的 DI,第一次讀到值時查本月與上月未解除的同名「警告」列:仍超限 → 沿用原列(不新增),
  已恢復 → 2 秒後解除;查詢失敗時重試,所有感測器共用 5 次,用完後回到一般邏輯。
- 原本的「量程 90% 高限」警報(`數值異常`)、DI / 漏水 / MS300 警報、設定儲存與 Modbus server HR 寫入都不變。
- log:`[LimitAlarm] …`(回到範圍 / 2 秒內再超限 / 重啟接手)、`[SQL] Alarm inserted: … (limit alarm pt04 upper)`、
  `[SQL] Alarm resolved: …`。

**給 Mango 的測試方法**(模擬器或實機皆可,不需改程式):

1. 開桌面版(或網頁),在主畫面記下某支感測器目前的值,例如 PT-04 = 120.00 kPa。
2. 到設定頁「設備接口出口壓力」把上限設成比目前值小(例如 100)後套用 → 主畫面的值超過上限;
   警報頁立刻出現 `PT-04`、`超過上限：120.00 kPa（上限 100.00 kPa）`、嚴重程度「警告」、狀態「未處理」
   (PT-05 若也超過,另外一筆 `PT-05`)。Filter 壓差可用「Filter 壓差」的上下限測,主畫面 Filter 會變紅(超上限)/ 橘(低於下限)。
3. 等幾秒:同一筆不會重複出現。
4. 把上限改回比目前值大(或清空 = 停用)後套用 → 約 2 秒後該筆變成「已解除」(綠色)。
5. 下限同理(設成比目前值大 → 「低於下限：…」)。
6. 重啟測試:保持超限時關閉程式再開 → 警報頁仍是原來那一筆「未處理」,不會多一筆;恢復後 2 秒變「已解除」。

測試:`Core/tests` 的 `tst_limit_alarms`(見「測試 / 驗證」5j)。不經瀏覽器的 live 測試工具:
`docs\evidence\w2-085\tools\`(`build-live-tools.bat` 建 Proxy Mirror 測試用戶端與資料庫列印工具,`live-run.ps1`
用自己的設定檔 / port / 資料資料夾啟動桌面版兩次並逐項檢查)。

## 存檔時套用 offset(w2-086)

Mango 2026-10-02 決定:設定頁的 Offset **存檔時就加上**,資料庫、歷史頁、CSV、REST 全部一致;Modbus server 給外部 HMI 的
PV 也套用。範圍表與欄位對照表見 `Core/SensorOffset.md`,契約見 `Core/SensorSettings.md`。

- 程式:`Core/SensorOffsetStorage.{h,cpp}`(新檔,只編進桌面版),`Manager` 只做接線(建構時建立、`saveServerInputData`
  存檔前換算整筆、`mirrorClientData` 鏡像到 Modbus server input register 時換算)。資料庫結構、SqlManager、Proxy 的 PV
  (仍是原始值,畫面自己加 offset)、原本的 90% 高限警報(原始值)、設定頁的 HR11~40、DI/DO/線圈都不變。
- 換算:`存入值 = round(原始計數 + offset ÷ 縮放比例)`,夾在 0~65535(夾到邊界時 warning,每欄每 60 秒最多一行);
  縮放比例取自 `Core/ModbusMapping.h`(TT ×100/65535 °C、PT ×1000/65535 kPa、流量 ×1 L/min)。流量 1 個計數 = 1 L/min,
  所以流量 offset 存檔時四捨五入到整數 L/min。
- 每一筆都用當下的設定:套用後下一筆(≤ 1 秒)就用新 offset;舊資料不改寫(改 offset 的時間點歷史曲線會跳一下)。
- log:`[SensorOffset] offsets in use from the next sample: …`(設定改變時一行)、
  `[SensorOffset] sensor_data sample with offsets: s5 pt01 raw=… stored=…`(有 offset 時每筆一行)、
  `[ModbusServer][Mirror] inputRegister=… raw=… server=…`(server = 給外部 HMI 的值)。

**給 Mango 的測試方法**(模擬器或實機皆可,不需改程式):

1. 主畫面記下 PT-01 目前的值,例如 500.00 kPa。
2. 設定頁把 PT-01 的 Offset 設成 12.5 後套用 → 主畫面 PT-01 變成 512.50。
3. 等 2 秒後到歷史頁按「篩選」(或下載 CSV):最新一筆的 PT-01 ≈ 512.50(與主畫面差不超過 0.01);
   套用之前的資料仍是約 500.00(舊資料不改寫)。
4. REST:瀏覽器開 `http://<IP>/api/sensor/last`,`s5` × 1000 ÷ 65535 ≈ 512.50。
5. 外部 HMI(或 Modbus 測試工具)讀 Modbus server input register 4:值 × 1000 ÷ 65535 ≈ 512.50。
6. Offset 改回 0 後套用 → 之後存的資料回到原始值。

測試:`Core/tests` 的 `tst_offset_storage`(見「測試 / 驗證」5j)。不經瀏覽器的 live 測試工具:`docs\evidence\w2-086\tools\`
(`build-live-tools.bat` 建 Proxy Mirror 測試用戶端,`live-run.ps1` 用自己的設定檔 / port / 資料資料夾啟動桌面版並逐項檢查)。

## 設備離線提示(w1-087 / w2-087)

連不上的設備會在桌面與網頁畫面上方顯示**橙色橫幅**「設備離線：ADAM-6217（192.168.1.203）、MS300（COM2）」(依下表順序,
以頓號分隔)。只是提示:不停用任何按鈕,其他設備照常操作。網頁與桌面的連線中斷時(紅色「離線」/「連線中斷」橫幅)那條優先,
設備橫幅暫時不顯示(資料可能已過時),連線恢復後自動回來。

- 介面(main,w1-087):`Core/TaidaFlowProxy.h` 的 `deviceStatus`(鏡像到所有網頁)、`TaidaFlowContent/TopNav.qml` 的橫幅。
- 寫入端(core,w2-087):`Core/DeviceStatusPublisher.{h,cpp}`(新檔,只編進桌面版),由 `Manager` 建立與啟停
  (`Manager::start()` 先啟動它再連設備;`Manager::stop()`(`Core::shutdown` 呼叫)先停它、寫回空 map,再關設備)。
  `Ms300FaultReader` 只多了兩個訊號(`portOpenChanged`、`faultStatusReadFailed`)。

| key | 設備 | 名稱(name) | 位址(address) | 離線的判斷 |
|---|---|---|---|---|
| `adam6256` | ADAM-6256 | ADAM-6256 | config.json 的 host(模擬器模式為 127.0.0.201) | Modbus TCP 沒有連線(w2-072 的斷線期間) |
| `adam6217a` | ADAM-6217 A | ADAM-6217 | 同上(127.0.0.202) | 同上 |
| `adam6217b` | ADAM-6217 B | ADAM-6217 | 同上(127.0.0.203) | 同上 |
| `adam6224` | ADAM-6224 | ADAM-6224 | 同上(127.0.0.204) | 同上 |
| `adam6022` | ADAM-6022 | ADAM-6022 | 同上(127.0.0.205) | 同上 |
| `ms300` | MS300 變頻器 | MS300 | `devices.ms300.serialPort`(例如 COM2) | 序列埠打不開 / 被關閉,或埠開著但連續 3 次讀故障狀態沒有回應(變頻器沒電、線沒接) |

- 名稱只放型號(兩台 ADAM-6217 以位址區分);位址是程式實際使用的位址(config.json,或測試用 `TAIDAFLOW_DEVICE_PROFILE=simulator`
  置換後的 127.0.0.20x)。
- 啟動時是空 map(不顯示);每台設備**第一次連線嘗試有結果**後才出現該設備(第一次還在連的設備不會被標離線)。之後連線狀態
  改變才重寫整份 map(`sinceMs` = 改變的時刻);斷線期間每 3 秒的重連失敗不會重寫;連上立即恢復 online。MS300 埠打開後要等
  變頻器第一次回應才算 online。
- ADAM 拔線但 TCP 還沒斷(Windows 要等送出的請求重送逾時,約十幾秒到數十秒)時不會立刻顯示;程式關閉時寫回空 map。
- log(full log):`[DeviceStatus] started ...`、每次改變一行 `[DeviceStatus] ADAM-6217 127.0.0.210 (adam6217b): OFFLINE (not connected, first result)`
  與 `[DeviceStatus] write #<n>: {adam6022=online, ..., ms300=OFFLINE}`,關閉時 `[DeviceStatus] stopped: deviceStatus = {} (unknown)`。

**給 Mango 的測試方法**(不需改程式):

1. 正常接模擬器啟動(`scripts\run-simulator.ps1` → `scripts\run-desktop.ps1 -DeviceProfile simulator`):開發機沒有 COM2,
   幾秒後橫幅顯示「設備離線：MS300（COM2）」;ADAM 都連上,不在橫幅內。
2. 關掉模擬器 → 約 1~5 秒內橫幅變成「設備離線：ADAM-6256（127.0.0.201）、ADAM-6217（127.0.0.202）、ADAM-6217（127.0.0.203）、
   ADAM-6224（127.0.0.204）、ADAM-6022（127.0.0.205）、MS300（COM2）」;按鈕仍可按(寫入會被拒絕並記 log)。
3. 再開模擬器 → 下一次重連(3 秒內)ADAM 從橫幅消失,只剩 MS300。
4. 或只讓一台離線:複製 `deploy\dev\config.simulator.json`,把 `devices.adam6217b.host` 改成不存在的位址 `192.0.2.203`
   (保留給文件用、不會有設備的網段;安全探測對它判定 unreachable,可以啟動),以
   `run-desktop.ps1 -Config <該檔>`(不加 `-DeviceProfile`)啟動 → 該台第一次連線逾時後(Windows TCP 連線逾時約 20 秒,之前不顯示)
   橫幅為「設備離線：ADAM-6217（192.0.2.203）、MS300（COM2）」。(w2-087 的 live 測試用的是本機沒人聽的 `127.0.0.210`,
   連線被拒,約 4 秒內出現;但 `run-desktop.ps1` 的安全探測會因本機位址沒有模擬器在聽而拒絕啟動,所以手動測試用 192.0.2.203。)
5. 網頁(`http://<IP>/`)同時顯示相同的橫幅;拔掉網頁電腦的網路時改顯示紅色「連線中斷」,插回後橙色橫幅回來。

測試:`Core/tests` 的 `tst_device_status` 與 `tst_device_status_simulator_profile`(見「測試 / 驗證」5j)。不經瀏覽器的 live
測試工具:`docs\evidence\w2-087\tools\`(`build-live-tools.bat` 建 Proxy Mirror 測試用戶端,`live-run.ps1` 用自己的設定檔
(ADAM-6217 B 指到 127.0.0.210、MS300 COM2)啟動桌面版並讀 deviceStatus)。

## 測試 / 驗證(全部以 exit code 判定)

QTest:`Core/AppHttpServer/tests`(可重用 HTTP 單例,只編該類別,見 5c)、`App/tests`(config.json 讀取器、`/runtime.json`
與程式自寫 log,見 `App/tests/README.md`)、5b 的歷史/匯出 harness、5e 的各連線端歷史檢視 harness、5f 的 DI 警報與 5i 的
SqlManager 關閉、5j 的 `Core/tests`(Core 單元測試:REST 分頁、schema、匯出狀態表、警報頁檢視、超限警報、存檔時套用 offset、
設備離線提示);
其餘整合以下列可重跑檢查驗證。`PS` = `powershell -NoProfile -ExecutionPolicy Bypass -File`。

```bat
:: 0. pack 與 1.0.1 來源逐檔一致、MANIFEST 24/24
PS scripts\verify-pack.ps1 -Source ..\WebAssemblyTest\integration-pack\wasm-mirror
:: 0b. pack 自帶 native tests(WasmMirror.Engine / Runtime)
scripts\run-pack-tests.bat
:: 1. fresh 建置(依序;桌面版同時產生 build\desktop\nginx,網頁版 configure log 有 [qml-scan])
scripts\build-desktop.bat fresh
scripts\build-wasm.bat wasm-release fresh
:: 2. WASM 不含後端(來源、Qt 模組、字串);desktop 含
PS scripts\check-wasm-backend.ps1 build\desktop build\wasm-release
:: 3. VERSION 地雷未觸發(兩邊 version_shadow_hits=0)
PS scripts\check-version-shadow.ps1 build\desktop build\wasm-release
:: 4. 字型子集涵蓋所有來源字元(選用工具,需要 Python + fonttools,docs\BUILD.md §2.5)
PS scripts\make-font-subset.ps1 --check
:: 5. desktop 啟動(安全探測、config.json 的 dataDir、後端 + mirror 127.0.0.1:18125 + 區網轉發 0.0.0.0:8125
::    (同一 app PID)+ 8124 HTTP 服務、網頁 200、runtime.json no-store、REST 127.0.0.1:18080 GET / 200、
::    WM_CLOSE 關閉 exit 0、之後無殘留;期望值都從 -Config 的設定檔讀)
PS scripts\verify-desktop-startup.ps1
::    接模擬器:先 scripts\run-simulator.ps1,再加 -DeviceProfile simulator [-ProbeLog <檔案>]
:: 5a. 安全探測自我測試(default / simulator / simcfg 三種設定,真的起模擬器與假 listener)
PS scripts\probe_selftest_sim.ps1
:: 5b. (w2-041) 歷史區間 + CSV 匯出的 QTest(編譯真的 SqlManager / HistoryExport / Proxy 原始碼):
::     先做 30 天、兩個月份檔的測試資料(build\w2-041-bench;w2-062 的 C++ 產生器,與原本的產生演算法相同),
::     再建置並跑 CTest(需 8124 空著)。harness 在 docs\evidence\w2-049\tools(w2-041 的原版留作歷史證據)
docs\evidence\w2-062\tools\make-bench-db.bat
docs\evidence\w2-049\tools\run-w2041-qtest.bat
docs\evidence\w2-045\tools\run-qtest.bat
:: 5c. (w2-049) AppHttpServer 單例的獨立 QTest(靜態 200/304/gzip/MIME/HEAD/COOP-COEP、穿越攻擊、
::     下載掛載、自訂路由、綁定失敗、多執行緒註冊、1 GiB 串流記憶體、w2-062 單檔 Cache-Control;需約 2 GiB 暫存磁碟空間)
scripts\run-apphttpserver-tests.bat
:: 5d. (w2-062/w2-064) config.json 讀取器、/runtime.json 與程式 log 檔的 QTest(App/tests:tst_appconfig、
::     tst_runtimeinfo、tst_applog;w2-082 加上介面字型的 tst_uifont、tst_uifont_wasmpath,見 App\tests\README.md;
::     建置 + CTest 的一行指令,或 App\tests\run-app-tests.bat fresh:)
docs\evidence\w2-065\tools\run-app-tests.bat
:: 5d2. (w2-065) 腳本的 config.json 讀取器(含 nginx.exe / log.* 鍵、log 資料夾解析、launcher / nginx access log 清理規則)
PS docs\evidence\w2-065\tools\test-config-reader.ps1
:: 5e. (w2-052) 各連線端獨立的歷史檢視 QTest(編譯真的 HistoryViews / SqlManager / HistoryExport / Proxy):
::     tst_w2052_views(多端交錯請求 = 單端結果、互不作廢、revision 只變自己的、閒置移除、上限 32、
::     不限區間、非法輸入)與 tst_w2052_rangepage(w2-045 正確性測試改走每端 API + 各端錨點)。
::     需 5b 的測試資料;不用任何 port。
docs\evidence\w2-052\tools\run-qtest.bat
:: 5f. (w2-053) DI 警報跨重啟的 QTest(不用設備、不用 port)
docs\evidence\w2-053\tools\run-qtest.bat
:: 5g. 正式機打包資料夾:打包(exit 0)→ DLL 相依(exit 0)→ 自動啟動乾跑(-WhatIf,不註冊)→ 實機檢查
::     (verify-release-package:第一次啟動建立 config.json / 資料資料夾、install-nginx-config + start nginx、
::     80 的網頁 / runtime.json / /mirror / REST / 匯出下載與 Range、搬移資料夾後自動重產 nginx.conf、壞 JSON exit 2、
::     還原出貨狀態)。-SeedDb 是測試資料(開發機沒有設備,歷史資料表是空的)。
PS scripts\package-release.ps1 -Force
PS scripts\check-package-deps.ps1 -Package dist\TaidaFlow-<日期>-<雜湊>
PS dist\TaidaFlow-<日期>-<雜湊>\register-autostart.ps1 -WhatIf
docs\evidence\w2-050\tools\build-mirror-client.bat
PS scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<日期>-<雜湊> -SeedDb build\runtime-cwd\data\sensor_202609.sqlite -FromMs 1790577423000 -ToMs 1790581023000
PS scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<日期>-<雜湊> -Mode bat
:: 5h. REST API:log 的 route 表與 RESTManager 實際註冊的一致(不用啟動 app)
PS scripts\check-rest-routes.ps1
:: 5i. (w2-067) SqlManager::shutdown() 的 QTest(關閉流程最後一步:寫入、關閉並 join、之後的呼叫被拒、資料檔完整、
::     第二次 shutdown 無作用;不用設備、不用 port,輸出 build\w2-067-qtest)
docs\evidence\w2-067\tools\run-qtest.bat
:: 5j. (w2-071 起) Core 單元測試 Core\tests(QTest + CTest,編譯真的 Core 原始碼,資料在暫存資料夾,
::     REST 測試用系統挑的空 port,不用設備;輸出 build\core-tests):
::     tst_rest_range_paging(6 個 range 路由分頁格式、page/pageSize > 2147483647 → 400、from/to 超界 1 秒內 400)、
::     tst_sqlmanager_schema_once(每個月份連線只跑一次 schema、「Schema file not found」整個執行期間最多一行)、
::     tst_historyexport_status(匯出狀態表上限:不合法 id 不存、拒絕項目與完成項目一起清理、忙碌項目不遺失)、
::     tst_alarm_views(w2-080 警報頁各端檢視:跨兩個月份檔的總數 / 頁數 / 未處理數、新到舊排序(同時間依 id)、序號、換頁與頁數夾限、
::     空區間、rows 與 alarmRecords 同一筆比對、新增 / 解除警報後自動更新且範圍外不變、兩端互不影響、同端只留最新、
::     不合法輸入、閒置 30 分鐘與最多 32 個的清理、5000 筆月份分步讀取)、
::     tst_server_heartbeat(w2-084 伺服器心跳:啟動立即寫一次、真實 1 秒計時器 6.5 秒內 7 個值且間隔約 1 秒、值 = 當下 epoch ms、
::     在 Proxy 的執行緒寫、stop() 後 3.5 秒不再變化、注入時鐘 + 短間隔、其他執行緒忙碌不影響、非主執行緒 start 被拒、
::     core.cpp 的接線(init 最後啟動、shutdown 第一步停止)與只編進桌面版、沒有其他程式寫 serverHeartbeatMs)、
::     tst_limit_alarms(w2-085 設定頁上下限超限警報:超限立即新增、等於上下限與停用不判斷、回到範圍 1.9 秒內再超限不解除不新增、
::     持續 2 秒才解除(注入時鐘與真實計時器)、同組感測器各自記錄、Filter = 校正後 PT-02 − PT-03、offset 生效且 PV 保持原始、
::     設定改變 / 停用立即重新判斷、重啟接手不重複新增與查詢失敗重試(共用次數)、嚴重程度「警告」、alarmRecords / alarmViews 狀態、
::     以 UI 自己的 JavaScript(SensorUnits.js 的 SensorUnits.limitState,Main.qml 的 Filter 與 w1-088 起所有數值共用)逐一比對判斷結果、真的 Manager 對測試自己的 Modbus TCP
::     server 走一遍(含原本 90% 高限警報不變)、manager.cpp / core.cpp / CMake 接線)、
::     tst_offset_storage(w2-086 存檔時套用 offset:key ↔ 欄位 ↔ 縮放對照、正負 / 0 / 未設定 / 非有限值、四捨五入、夾限 0 與 65535
::     與 warning 限流、壓力 / 溫度 / 流量、設定改變後下一筆立即用新 offset、歷史換算值與 UI 自己的 SensorUnits.adjusted() 逐一比對、
::     真的 Manager 對測試自己的 Modbus TCP server:資料庫列、歷史頁、CSV 匯出、REST /api/sensor/last、Modbus server input
::     register 都是校正後的值,Proxy PV 與 90% 高限警報仍用原始值,DI 線圈不變、manager.cpp / CMake 接線)、
::     tst_device_status 與 tst_device_status_simulator_profile(w2-087 設備離線提示 deviceStatus:同一個測試程式跑兩次,第二次設
::     TAIDAFLOW_DEVICE_PROFILE=simulator;四台 ADAM 對測試自己的 Modbus TCP server、ADAM-6217 B 指到沒人聽的本機位址、MS300 用
::     不存在的 COM 名稱:啟動時空 map、每台第一次連線有結果前沒有該 key、之後完整 6 個 key、name / address(含 simulator 置換後位址)、
::     重連失敗不重寫、斷線只寫一次、恢復寫 true、stop 後空 map;MS300 讀取規則;真的 Manager start / stop;core.cpp / manager.cpp /
::     CMake 接線)
Core\tests\run-core-tests.bat fresh
:: 5k. (w2-084) 網頁載入頁的當機自動重新整理(node,不開瀏覽器;node 用 emsdk 附的 node.exe):inline script 語法、
::     退避函式與 TaidaFlowContent\components\LinkWatchdog.js 逐一比對、假時鐘 / 假 sessionStorage 下的倒數與退避、
::     以假 DOM 與假 qtLoad 執行頁面真正的 init():程式結束、下載失敗、不支援 WebAssembly、未捕捉 RuntimeError、
::     一般腳本錯誤不動作、轉場中當機。加上建置好的頁面路徑時也檢查該頁(與樣板同一段程式)
App\wasm\tests\run-shell-tests.bat build\wasm-release\TaidaFlowApp.html
```

- 以前各輪的專用檢查工具(w2-043 區網轉發、w2-050 nginx / 1 GB Range、w2-060 REST 全表)在各自的 `docs/evidence/<輪次>/tools/`,
  是當時的證據,**不再列為必跑步驟**(部分依賴已移除的環境變數或工具);上面的清單涵蓋它們在現行設計下的檢查。

E2E 證據(雙向同步 double/bool、唯讀 PV 單向、斷線離線提示與控制項停用、重連恢復、
中文顯示)在 `docs/evidence/wasm-v4/`,檔名即步驟說明。CSV 匯出(desktop 存檔、網頁下載、
離線匯出、內容比對)在 `docs/evidence/wasm-v4-csv/`。
