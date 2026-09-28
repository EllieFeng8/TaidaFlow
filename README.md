# TaidaFlow

> **編譯(給開發人員):[docs/BUILD.md](docs/BUILD.md)**
> ——從 clone、要裝的軟體與版本、工具路徑,到桌面版 / 網頁版的腳本與不用腳本的手動編譯、Qt Creator、編譯後必跑的檢查、
> main → core 合併後要做的事、常見問題。
>
> **部署與啟動(正式機 / 測試機,給操作人員):[docs/DEPLOY_AND_STARTUP.md](docs/DEPLOY_AND_STARTUP.md)**
> ——要開哪個檔、怎麼開、不用腳本的手動 cmd 步驟、port 表、防火牆、常見問題。打包時複製成打包資料夾的 `DEPLOY.md`。

TaidaFlow 是 Qt Design Studio 產生的 Qt Quick HMI(主畫面 / 警報 / 歷史)。本分支 `core`
含真實後端 `Core/`:`core.cpp`、`manager.cpp`、`Modbus_Client`(5 台 ADAM,預設
`192.168.1.201~205:502`,會寫 DO/AO;**測試用**環境變數 `TAIDAFLOW_DEVICE_PROFILE=simulator`
改連本機模擬器 `127.0.0.201~205:502`,見「接 Adam60xxSimulator」)、`Modbus_Server`(bind `AnyIPv4:502`)、
`Ms300FaultReader`(Modbus RTU `COM2`)、`RESTManager`(REST API,w2-060 起啟用:`127.0.0.1:18080`,
區網經 nginx `http://<IP>/api/...`,見「REST API」)、`SqlManager`(SQLite)、
`HistoryViews`(各連線端獨立的歷史檢視)與 `HistoryExport`(歷史 CSV 匯出佇列,兩者見「歷史資料:時間區間與匯出」)、`AppHttpServer`(可重用的
HTTP 伺服器單例,`0.0.0.0:8124` 同時提供**網頁**與 **CSV 下載**,見「網頁與下載(HTTP 8124)」)。

QML 只面對 `Core/TaidaFlowProxy.h`(QML singleton `Td`,module URI `TaidaFlowBackend`)。
透過 `integration-pack/wasm-mirror`(pack **1.0.1**,wire protocol 3,**唯讀、由維護方提供**,
整包複製,`MANIFEST.sha256` 24/24),同一份程式可編成:

- **Desktop(Windows MSVC)**:authoritative 端。`Core::instance().init()` 建立並擁有
  `TaidaFlowProxy`、啟動全部硬體後端,Mirror server 綁 `ws://127.0.0.1:18125/mirror`(內部 port,
  只限本機),並由 `App/lanrelay.h` 在 `0.0.0.0:8125` 轉發給它(見「區網連線(mirror)」)。
- **WebAssembly**:replica 端。**不編譯任何後端**(Desktop-only Core,pack 文件
  package-integration §1/§5.4、guide §10/§11),`main.cpp` 直接建立 `TaidaFlowProxy` 當 replica,
  瀏覽器頁面連回**載入頁面的那台主機**(`location.hostname`)的 `8125`,雙向同步 `Td` 的全部 Q_PROPERTY。

## 安全注意(開 desktop 前必讀)

desktop 版會對 `192.168.1.201~205:502` 建 Modbus TCP 連線並**寫入** DO/AO(泵浦、閥、變頻器、
緊急停止迴路),開 `COM2`,並在 `0.0.0.0:502` 開 Modbus server。在非現場的開發機上:

- 一律用 `scripts\run-desktop.ps1` 啟動。它先跑 `scripts\safety_probe.ps1`(對 5 台 ADAM 只做
  TCP connect、1.5 秒逾時、**不送任何 Modbus**;列出本機序列埠;檢查 502/8124/8125/18125 與 REST port
  (18080 或 `TAIDAFLOW_REST_PORT`)是否已有人 listen),任一裝置可達、出現 `COM2` 或 502 已被占用 → **不啟動**(exit 3);8125(mirror
  區網轉發)、18125(內部 mirror)、8124(網頁 + CSV 下載)或 REST port 已被占用 → 不啟動(exit 4 `BUSY`,
  不會關掉別人的程式)。80 與 8123(nginx 網頁前端:w2-057 起預設 80,之前是 8123)只列出、**僅供參考,不阻擋啟動**(nginx 只送
  靜態檔,不碰設備;app 也不綁這兩個 port;占用者是 pid 4 時註明為 Windows HTTP.sys)。每次探測都附加寫入 `docs/evidence/wasm-v4/safety-probe.log`(`-LogFile` 可改)。
- desktop 另在 `0.0.0.0:8124` 開 HTTP 服務(網頁 + CSV 下載)、在 `0.0.0.0:8125` 開 mirror 區網轉發
  (內網、都不做存取控管;只提供網頁資料夾的網頁檔與匯出資料夾裡的檔,見下方),並在 `127.0.0.1:18080`
  開 REST API(只限本機;區網經 nginx 的 `/api/`,可用 PUT 改設定,不做存取控管,見「REST API」)。
- 工作目錄固定為 `build\runtime-cwd\`:Core 會在「目前工作目錄」寫 `TaidaFlowSettings.ini`、
  `settings.sqlite`、`data\sensor_YYYYMM.sqlite`(及 REST 用的 `device_info.ini`),網頁匯出的 CSV
  寫在 `exports\`。`build/` 已被 `.gitignore` 排除,work tree 保持乾淨。
- 不要為了接模擬器改網路設定(不加 IP alias)或改 Core 程式;要接模擬器請用下方
  「接 Adam60xxSimulator」的測試用 profile。`run-desktop.ps1` 在預設模式會**移除**
  `TAIDAFLOW_DEVICE_PROFILE`,確保 app 用的就是探測過的 192.168.1.x。

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
powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -Label "manual"   # 先探測再啟動
# 瀏覽器開 http://127.0.0.1:8124/TaidaFlowApp.html(區網:http://<desktop 的區網 IP>:8124/TaidaFlowApp.html)
```

- 網頁由 **desktop 自己**提供(`AppHttpServer` 單例,與 CSV 下載同一個 `0.0.0.0:8124`),不需要另開
  網頁伺服器。網頁檔資料夾的決定順序見「網頁與下載(HTTP 8124)」;開發機上 `build\wasm-release`
  建好就直接用,正式部署用 `scripts\deploy-web.ps1` 複製到 `<exe 資料夾>\web`。
- `run-desktop.ps1` 設 `QT_FORCE_STDERR_LOGGING=1`、PATH 加 Qt bin,log 寫到
  `build\runtime-logs\`。啟動 log 應含 `[ModbusServer] listening on 0.0.0.0:502 unit=1`、
  `WASM Mirror endpoint: ws://127.0.0.1:18125/mirror`、
  `LAN relay listening: 0.0.0.0:8125 -> 127.0.0.1:18125`、
  `[Web] web page folder (<來源>): <資料夾> -> http://<host>:8124/TaidaFlowApp.html` 與
  `[Web] HTTP service listening on 0.0.0.0:8124`(8124 被占用時只記 warning,app 照常執行)。
- 瀏覽器 console:`WASM Mirror ready: true` 即同步完成。

### 網頁載入畫面(w2-058)

- 樣板:`App/wasm/TaidaFlowApp.shell.html`(純 HTML/CSS,重現 CubeLoader 旋轉發光立方體 + 「TAIDAFLOW」
  與繁中狀態文字;不引用任何外部 CSS/JS/字型)。`onLoaded` 後整個載入頁隱藏,由 App 自己的畫面接手;
  載入失敗、瀏覽器不支援 WebAssembly、JavaScript 關閉與程式結束時停在載入頁顯示繁中訊息(紅字)。
- 套用:Qt 6.8 沒有自訂 HTML shell 的 CMake 參數,它在 CMake configure 時從自己的 `wasm_shell.html`
  產生 `build\<wasm preset>\TaidaFlowApp.html`。`App/CMakeLists.txt` 在同一次 configure、Qt 產生之後
  立刻呼叫 `App/wasm/apply_wasm_shell.cmake`,以 Qt 產生的頁面取 `@APPNAME@`/`@APPEXPORTNAME@`/
  `@PRELOAD@` 的值填入樣板並覆蓋;configure log 會印 `[wasm-shell] ...`(值與 SHA-256)。只影響 WASM,
  桌面版不變。`qtlogo.svg` 仍由 Qt 複製、仍會被 deploy-web 部署,新頁面不再使用(留著無影響)。
- 修改:改樣板 → `scripts\build-wasm.bat`(樣板是 configure 相依,改了會自動重跑 configure)→
  `powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1`(nginx 執行中另跑
  `scripts\nginx-web.ps1 -Action reload`,或靠 ETag 重新驗證)。靜態檢查(不開瀏覽器):
  `python -B docs\evidence\w2-058\check_shell_page.py build\wasm-release\TaidaFlowApp.html`。
- main 分支的 wasm 建置仍用 Qt 預設頁;要一致需把 `App/wasm/` 與 `App/CMakeLists.txt` 的 w2-058 區塊
  一起帶到 main(main → core 合併時這兩處只在 core,不會被覆蓋,除非 main 也改了同一段)。

### 網頁與下載(HTTP 8124,w2-049)

desktop 的 Core 只透過 `AppHttpServer::instance()`(`Core/AppHttpServer/`,只依賴 Qt Core/Network/
HttpServer 的可重用類別,用法見該資料夾的 `README.md`)提供 HTTP,單例跑在自己的執行緒
(`AppHttpServerThread`),大檔傳送不占 UI 執行緒。`Core::startHttpServer()` 在 `Core::init()` 掛上網頁
(`/`)後 `start(8124, AnyIPv4)`;`HistoryExportManager` 掛上 `/exports`;`aboutToQuit` 時 `stop()`。

- **網頁檔資料夾**(第一個含 `TaidaFlowApp.html` 的):環境變數 `TAIDAFLOW_WEB_DIR` →
  `<exe 資料夾>\web` → 開發預設 `<repo>\build\wasm-release`(編譯時寫入,不存在就跳過)。
  每個候選與結果都記 log(`[Web] candidate ...`、`[Web] web page folder (...)`);都找不到只記
  warning(`[Web] no web page folder found ...`),`/exports` 下載與其他功能照常。
- **部署**:`powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1 [-Source build\wasm-release]
  [-ExeDir build\desktop]` 清空 `<exe 資料夾>\web` 後複製同一次 build 的網頁檔(不含 CMake/Ninja 檔),
  並為 `.wasm/.js/.html/.svg` 等產生 `.gz`(修改時間與原檔相同)。
- **行為**(取代舊的 Python 開發伺服器,符合 `integration-pack/wasm-mirror/docs/http-server-requirements.md`):
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
  `downloadPort` 預設 8124(本服務);app 以 `TAIDAFLOW_DOWNLOAD_PORT=80`(nginx 的 port)啟動時改由 nginx 送檔,
  見下一節。8124 的 `/exports` 一直保留,作為備援。

### nginx 網頁前端與下載(port 80,w2-050 / w2-057)

nginx 是**另一個程式**,取代舊的 `serve_wasm.py` 做網頁前端:從部署好的網頁資料夾送網頁,並**直接從
app 的匯出資料夾**送歷史 CSV(下載不經過 Qt、支援續傳)。app 內建的 8124 照常運作,兩個網址都能用:

- `http://<IP>/`(nginx,port 80,`/` → 302 `/TaidaFlowApp.html`;w2-057 起預設 80,`-Port 8123` 可改回舊 port)
- `http://<IP>:8124/TaidaFlowApp.html`(desktop app 內建 `AppHttpServer`)

**安裝 nginx**(一次,repo 外):從 <https://nginx.org/en/download.html> 取 **Stable** 版 Windows zip
與 `.asc`,以 <https://nginx.org/keys/> 的金鑰用 gpg 驗簽(Git for Windows 內附
`C:\Program Files\Git\usr\bin\gpg.exe`,`gpg --verify nginx-<版本>.zip.asc nginx-<版本>.zip` 要是
`Good signature`),解壓到 `C:\tools\nginx\nginx-<版本>\`。目前用 **1.30.5**(SHA-256
`e5afe28b6a50bec92c478bfe1a4d3758206b80fb77159277bc5c4e88955c2a35`,2,776,622 bytes,
簽章者 Sergey Kandaurov `D6786CE303D9A9022998DC6CC8464D549AF75C0A`;證據 `docs/evidence/w2-050/01-*`)。

**部署與啟動**:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1        # build\wasm-release -> <exe>\web + .gz
# app 以 nginx 的 port(80)當下載連結的 port(不設就維持 8124);run-desktop.ps1 會把環境變數帶給 app
$env:TAIDAFLOW_DOWNLOAD_PORT = '80'
powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -Label "with nginx"
powershell -ExecutionPolicy Bypass -File scripts\nginx-start.ps1        # 0.0.0.0:80(-Port 8123 改用舊 port)
# ...
powershell -ExecutionPolicy Bypass -File scripts\nginx-stop.ps1         # nginx -s quit(只停自己起的)
```

- 設定檔 `deploy/nginx/taidaflow.conf` 是**樣板**;`scripts\nginx-web.ps1`(`nginx-start.ps1` /
  `nginx-stop.ps1` 是它的 `-Action start|stop` 捷徑,另有 `reload`、`test`、`status`)把
  `@TAIDAFLOW_WEB_ROOT@`(預設 `<exe 資料夾>\web`,`-WebRoot` 覆寫)與 `@TAIDAFLOW_EXPORT_DIR@`
  (= app 工作目錄 `\exports`,預設 `build\runtime-cwd\exports`,`-ExportDir` 覆寫)、`@TAIDAFLOW_NGINX_PORT@`(`-Port`)與
  `@TAIDAFLOW_REST_PORT@`(w2-060,`/api/` 轉送的 REST 內部 port,`-RestPort` 或 `TAIDAFLOW_REST_PORT`,預設 18080)填入後寫到 runtime
  資料夾(預設 `build\nginx\`:`conf\`、`logs\`、`temp\`、狀態檔 `taidaflow-nginx.json`),先
  `nginx -t`,通過才以 `nginx -p <runtime>/ -c <runtime>/conf/taidaflow.conf` 啟動。nginx 本體的資料夾
  不被寫入。
- 找 nginx.exe:`-Nginx <exe 或資料夾>` → 環境變數 `TAIDAFLOW_NGINX` → `C:\tools\nginx\nginx-*\`
  最新版。
- 腳本規則與 exit code:0 完成;1 沒有本腳本起的 nginx 在跑(stop/reload/status);2 找不到 nginx /
  網頁資料夾 / `TaidaFlowApp.html`,或路徑含設定檔不能用的字元;3 `nginx -t` 失敗(不啟動);4 port(預設 80)已被
  占用(不啟動,也不關掉占用者)或已在跑;5 網頁資料夾或匯出資料夾是/含有符號連結、junction;6 起來但
  時限內沒 listen(已停掉);7 停止失敗。**停止只針對本腳本起的那個 nginx**(狀態檔的 pid + 映像檔路徑 +
  啟動時間 + `logs\nginx.pid` 都要相符),用 `nginx -s quit`(時限內沒結束才 `-s stop`);別的 nginx 一律不動。
- 網頁規則與 app 的 8124 相同:MIME(`.wasm` = `application/wasm`、`.html` / `.js` 帶
  `charset=utf-8`)、只送網頁檔副檔名(`html js mjs wasm css json map svg png jpg ico ttf otf woff
  woff2`,其他如 `CMakeCache.txt`、`build.ninja`、`.gz` 本身 → 404)、`.` 開頭 404、不列目錄、
  `/` → 302 `/TaidaFlowApp.html`、`etag on` + `Cache-Control: no-cache`(HTML 與其他檔都每次重新
  驗證,新 build 一定拿到新版)、`gzip_static on` 送 `deploy-web.ps1` 產生的 `.gz`(`Vary:
  Accept-Encoding`)、COOP `same-origin` / COEP `require-corp` / CORP `same-origin`、
  `server_tokens off`(錯誤頁只有 `nginx`,不含版本與路徑)。路徑穿越由 nginx 先正規化(`..`、`%2e`、
  `\`)再比對,離開根目錄 → 400。
- **Windows 版 nginx 沒有 `disable_symlinks`**(nginx.org 文件:只在有 `openat()`/`fstatat()` 的系統;
  1.30.5 `nginx -t` 回 `unknown directive`),會跟隨網頁資料夾裡的 junction(實測見
  `docs/evidence/w2-050/live/`)。所以 `nginx-start.ps1` / `reload` 會掃描網頁資料夾與匯出資料夾,有
  reparse point(符號連結、junction)就拒絕(exit 5);`deploy-web.ps1` 也不會刪除含連結的舊網頁資料夾
  (避免 `Remove-Item` 跟著刪到目標),複製後再掃一次。nginx 執行中才建立的連結掃不到:`/exports` 內名稱
  像匯出檔的**資料夾**(junction)一律 404;檔案符號連結需要系統管理員權限才能建立,不在防護範圍。
- nginx 在 Windows **不是服務**,開機不會自己起來;本專案不註冊服務。正式機(w2-057)由 `start-taidaflow -UseNginx`
  與 app 一起啟動,開機自動啟動用打包資料夾的 `register-autostart.ps1`(工作排程器「使用者登入時」,先 `-WhatIf`;
  為何不用服務、WinSW / NSSM 做法見 `docs/DEPLOY_AND_STARTUP.md` §1.6)。
- Windows 首次有程式 listen `0.0.0.0` 時可能跳出防火牆詢問視窗;腳本不改防火牆。

**下載(`/exports/`,nginx 直送)**:

- 只送 `^/exports/[A-Za-z0-9_-]{1,40}_\d{8}_\d{6}\.csv$`(與 Core 的 `isValidExportFileName` 相同),
  其他(子資料夾、別的副檔名、`..`、編碼過的斜線、太長的 sessionId …)一律 404。回應
  `Content-Type: text/csv; charset=utf-8`、`Content-Disposition: attachment; filename="<檔名>"`、
  `Cache-Control: no-store`、`Access-Control-Allow-Origin: *`,從磁碟串流(nginx 與 app 記憶體不隨檔案
  大小成長,實測見證據)。
- Core:`TAIDAFLOW_DOWNLOAD_PORT=<1..65535>` → `historyExportStatus` 的 `downloadPort`(網頁連結
  `http://<pageHost>:80/exports/<檔名>`,與不帶 `:80` 等價);不設 → 8124(原樣);不是合法 port → 記 warning、用 8124。
  log:`[Export] TAIDAFLOW_DOWNLOAD_PORT=80 - download links use port 80 (...)` 與
  `[Export] web export folder ... download links -> port 80`。
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

- **位址**:只綁 `127.0.0.1:18080`(內部 port,區網位址連不到);區網一律經 nginx:`http://<IP>/api/...`
  (`deploy/nginx/taidaflow.conf` 的 `location ^~ /api/` 原樣轉給 `http://127.0.0.1:18080`,方法、body、query、
  標頭都保留;`http://<IP>/api/` 本身轉到 REST 的狀態 route `/`)。port 用環境變數 `TAIDAFLOW_REST_PORT`
  (1..65535,不合法 → warning、用 18080)改;nginx 端用 `nginx-web.ps1 -RestPort`(或同一個環境變數),正式機
  `start-taidaflow.ps1 -RestPort` / `.bat` 的 `REST_PORT` 兩邊一起帶。18080 選擇理由:開發機上未被占用、不在
  Windows 保留範圍(`netsh int ipv4 show excludedportrange protocol=tcp` 只有 2869、50000-50059),也不與
  80/502/8124/8125/18125 衝突。
- **log**:`[REST] REST API listening on 127.0.0.1:18080 (...)` 與每個 route 一行 `[REST] route GET,PUT,OPTIONS /api/...`
  (表格在 `Core/core.cpp` 的 `kRestRoutes`,`python -B scripts\check_rest_routes.py` 比對它與 `RESTManager.cpp`
  實際註冊的 route,不一致 exit 1);RESTManager 自己另印 `Device SN: ...` 與 `Running on http://127.0.0.1: 18080 /`。
  綁定失敗(port 被占用)只記 `[REST] REST API NOT started on 127.0.0.1:18080 ...`,app 照常執行,nginx 的 `/api/` 回 502。
  關閉:`aboutToQuit` 時刪除 RESTManager(listener 關閉,log `[REST] REST API stopped`)。
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
| GET | `/api/sensor/range` | `sensor_data` 列 `[{"ts":..,"s1":..,...,"s40":..}]`(舊到新,可跨月) | `from`、`to`:epoch 秒(也接受 `123+60` / `123-60`) | 不改 |
| GET | `/api/holding/range` | `holding_register` 列(`h1..h100`) | 同上 | 不改 |
| GET | `/api/device/sn` | `{"sn":"sn000000"}` | — | `device_info.ini` 不存在時建立 |
| GET | `/api/sensor/last` | 本月最新一列;沒有 → 404 `{"ok":false,"error":"no data"}` | — | 本月資料檔不存在時建立(SqlManager 開檔) |
| GET | `/api/holding/last` | 同上(holding) | — | 同上 |
| GET | `/api/sensor/rangeDateTime` | 同 `/api/sensor/range` | `from`、`to`:ISO 8601(`2026-09-28T10:00:00` = 本機時間,結尾 `Z` = UTC)或 epoch 秒 | 不改 |
| GET | `/api/sensor/rangeDateTimePage` | `{"page","pageSize","totalCount","totalPages","hasPreviousPage","hasNextPage","items":[...]}` | 同上 + `page`(預設 1)、`pageSize`(預設 200,最多 1000) | 不改 |
| GET | `/api/holding/rangeDateTime` | 同 `/api/holding/range` | 同 `sensor/rangeDateTime` | 不改 |
| GET | `/api/holding/rangeDateTimePage` | 分頁版 | 同 `sensor/rangeDateTimePage` | 不改 |

- 每個 `/api/...` 路徑都有 `OPTIONS`(CORS 預檢,200 + 上述標頭);`/api/settings/frequency` 的 OPTIONS 被註冊兩次(原程式),
  第一個生效,行為相同。錯誤:參數/JSON 不合法 400 `{"ok":false,"error":"..."}`、SQL 失敗 500、沒有的路徑 404。
- 目前 Manager 存檔時**不寫** `holding_register`,holding 類 route 回空陣列 / 404 `no data`(原樣)。
- **注意**:RESTManager 在**主執行緒**(UI)執行,每個查詢以 blocking 方式等 SqlManager 執行緒;`/api/sensor/range` 與
  `/api/holding/range`、`rangeDateTime` 會一次回傳整個區間的所有列(不分頁),大區間(例如幾個月,每月約 260 萬列)
  會讓 UI 停住很久並佔大量記憶體。大量資料請用 `...Page` 版本(每頁最多 1000 列)。這是原程式行為,本輪沒有改。
- 實測(simulator profile、經 `127.0.0.1` 與區網 IP 的 nginx 80、PUT 寫入後讀回再寫回原值、區網直連 18080 被拒):
  `docs/evidence/w2-060/`(工具 `docs\evidence\w2-060\tools\rest_api_check.py`)。

### 區網連線(mirror,合併自 main e4bc327 / w2-042)

| 端點 | 綁定 | 用途 |
|---|---|---|
| nginx 網頁前端(`scripts\nginx-start.ps1` / 正式機 `start-taidaflow`,另一個程式) | `0.0.0.0:80`(`-Port` 可改,例如 8123) | 網頁 `http://<IP>/`;匯出檔 `/exports/<檔名>` 由 nginx 直接從匯出資料夾送(支援續傳);REST API `/api/...` 轉給 `127.0.0.1:18080` |
| REST API(`RESTManager`,desktop app) | `127.0.0.1:18080`(`TAIDAFLOW_REST_PORT`) | 內部 port,只限本機;區網經 nginx `http://<IP>/api/...`(見「REST API」) |
| 網頁 + CSV 下載(`AppHttpServer`,desktop app) | `0.0.0.0:8124` | 區網電腦開 `http://<desktop 的區網 IP>:8124/TaidaFlowApp.html`;匯出檔 `/exports/<檔名>`(備援,不支援續傳) |
| mirror 區網轉發(`App/lanrelay.h`) | `0.0.0.0:8125` | 網頁連的公開 mirror port;每條連線原樣雙向轉發到 `127.0.0.1:18125` |
| Mirror server(pack) | `127.0.0.1:18125` | 內部 port,只限本機,區網位址連不到 |

- 網頁版的 mirror host = 載入頁面的主機名稱(`location.hostname`),port `8125`;取不到時退回
  `127.0.0.1` 並記 warning。所以從哪個位址開網頁,就連回同一個位址的 8125。
- Mirror `allowedOrigins = {}`(空清單 = 不限制 Origin);內網系統,依 Mango 決定不做存取控管,
  轉發也不做來源限制。
- 別台電腦要連進來,Windows 防火牆需放行 **80/8125**(TCP 輸入;80 = nginx 網頁、下載與 REST `/api/`,8125 = 網頁同步;8124 只在要用備援網址 `:8124` 時開;
  18080 **不要開**,它只綁 127.0.0.1),
  外部 HMI 另需 **502**(建議只開給 HMI 的 IP;`netsh` 範例見 `docs/DEPLOY_AND_STARTUP.md` §1.9)。這由**管理員**設定,
  本專案的腳本不改防火牆 / 網路設定;首次啟動 Windows 可能跳出防火牆詢問視窗。
- 轉發是**暫時做法**:pack 1.0.0/1.0.1 只允許 Mirror 綁 loopback。等 wasm-mirror pack **1.0.2** 提供
  正式開關後,改由 Mirror 直接綁 `0.0.0.0:8125`,並刪除 `App/lanrelay.h` 與 `main.cpp` 中的使用。
- 8125 綁定失敗(例如被占用)時只記 `LAN relay could not listen on 0.0.0.0:8125: ...`,desktop 照常執行
  (本機的 Mirror 仍在 18125),只是網頁連不上。

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

## 正式機部署(w2-057)

操作人員版(要開哪個檔、手動 cmd 步驟、更新、備份、防火牆、常見問題、待決事項)在
**[docs/DEPLOY_AND_STARTUP.md](docs/DEPLOY_AND_STARTUP.md)**;打包時複製成打包資料夾的 `DEPLOY.md`。這裡只列開發者要知道的部分。

- **打包**(開發機):`powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-release.ps1 [-IncludeNginx] [-Force]`
  → `dist\TaidaFlow-<yyyyMMdd>-<git 短雜湊>[-dirty]\`(`dist/` 已加入 `.gitignore`)。`build\desktop` / `build\wasm-release`
  以 `ninja -n` 確認是最新,否則 exit 3。內容:`TaidaFlowApp.exe`;`windeployqt`(Qt 6.8.3,`--release --no-compiler-runtime
  --no-translations --skip-plugin-types qmltooling,canbus --exclude-plugins qsqlmimer,qsqlodbc,qsqlpsql`,`--qmldir`
  指向 `TaidaFlowContent`、`TaidaFlow`、`Dependencies`)帶入的 Qt DLL / plugins / `qml\`(QtQuick、Controls、Shapes、Timeline、
  Qt5Compat.GraphicalEffects、Qt.labs.qmlmodels …;專案自己的 QML 模組是靜態連結進 exe 的);**MSVC 執行環境採 app-local**:
  從建置用工具組對應的 `VC\Redist\MSVC\<版本>\x64\Microsoft.VC145.CRT` 複製 DLL(免安裝 vc_redist;要改用系統安裝版見
  DEPLOY.md);`web\`(同 `deploy-web.ps1`,含 `.gz`);`deploy\release\` 的正式機腳本、`logging\quiet.ini`、
  `scripts\nginx-web.ps1` + `deploy\nginx\taidaflow.conf`、`DEPLOY.md`、`VERSION.txt`、`MANIFEST.txt`(每檔大小與 SHA-256);
  `-IncludeNginx` 時 `nginx\`(nginx.exe + `docs\` 授權檔)。資料庫、ini、log、Python、`__pycache__`、`.lib/.pdb` 等一律不收
  (打包最後檢查,出現即 exit 6)。
- **DLL 相依檢查**:`scripts\check-package-deps.ps1 -Package <資料夾>`(`dumpbin /dependents`,每個 exe/dll 的匯入都要在打包資料夾或
  System32 找到;Qt 安裝目錄不算)。
- **正式機腳本**(`deploy\release\`,在打包資料夾最上層):`start-taidaflow.ps1/.bat`(**不做開發機安全探測**;`-DataDir`、
  `-LogDir`、`-UseNginx`、`-Port`(預設 80)、`-RestPort`(w2-060,預設 18080,設成 app 的 `TAIDAFLOW_REST_PORT` 並帶給 nginx 的 `/api/`)、
  `-AppLog quiet|full`;一律移除 `TAIDAFLOW_DEVICE_PROFILE`;PATH
  去掉含 `Qt6Core.dll` 的資料夾;單一執行個體與 502/8124/8125/18125/REST port/nginx port 占用檢查(列出占用者,含 pid 4 = HTTP.sys),
  不關別人的程式;`-UseNginx` 時 `TAIDAFLOW_DOWNLOAD_PORT` = nginx 的 port;確認 8124/8125 在聽,REST port 沒在聽只記 WARNING)、`stop-taidaflow.ps1/.bat`
  (先停自己起的 nginx,再以 WM_CLOSE 正常關 app;逾時只報告,`-Force` 才強制)、`register-autostart.ps1` /
  `unregister-autostart.ps1`(工作排程器「使用者登入時」;**先 `-WhatIf`**,本專案只做過乾跑)、`taidaflow-site.example.bat`。
- **開發機上驗證打包資料夾**:`powershell -ExecutionPolicy Bypass -File scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<...>
  [-Mode ps1|bat] [-SeedDb <sensor_yyyyMM.sqlite>]`:先 `safety_probe.ps1`(SAFE 才繼續)→ 以不含 Qt 的 PATH 啟動 → 載入的 Qt/MSVC DLL
  都來自打包資料夾 → curl 檢查 `http://<127.0.0.1 與區網 IP>/`(302)、頁面 200、`.wasm` gzip、8124、8125 WebSocket 101、
  REST(w2-060:`/api/` 與 `/api/settings/frequency` 200 + CORS、OPTIONS 預檢 200、18080 只綁 127.0.0.1 且區網 IP 直連被拒)→
  (dev-only mirror client `build\w2-057-mirror-client`)匯出 → `downloadPort` = nginx port、網頁 `exportDownloadUrl` 組出的連結
  200 + SHA-256 相同、Range 206 → app log 無 QML/plugin 載入錯誤 → 停止後無殘留。不操作畫面。
- **開發機腳本與正式機腳本不可混用**:開發機一律用 `scripts\run-desktop.ps1`(安全探測);正式機腳本沒有保護。

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
# 3. 網頁版:瀏覽器開 http://127.0.0.1:8124/TaidaFlowApp.html(desktop app 提供)
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
  與 `App/fonts/charset.txt`。目前的檔案大小、字元數與缺字以
  `python -B scripts\make_font_subset.py --check` 的輸出為準(README 不記數字,以免與字型不同步)。
  **新增中文字串後要重跑**;`--check` 在字元集變動或缺字時 exit 1。
- 來源字型:`C:\Windows\Fonts\NotoSansTC-VF.ttf`(Noto Sans TC 2.004,Windows 11 內附;亦可從
  <https://fonts.google.com/noto/specimen/Noto+Sans+TC> 下載 `NotoSansTC[wght].ttf` 以 `--source` 指定)。
  授權 SIL Open Font License 1.1,宣告保留於字型 name table。
- 載入:`App/embeddedfonts.cpp`。兩平台都 `addApplicationFont`;**只有 WASM** 建 fallback 鏈
  (`QFont::insertSubstitutions` + Han/Common script fallback)。desktop 顯示不變。

## 開發用工具(dev-only)

- w2-060 已移除測試專用的 PV 注入(舊的 `App/main.cpp` E2E 區塊與 `run-desktop.ps1` 的對應參數);無設備時改用
  Adam60xxSimulator(見「接 Adam60xxSimulator」)。`docs/evidence/` 下的歷史證據不變。
- `scripts\desktop_input.py`(OS SendInput / WM_CLOSE):`verify-desktop-startup.ps1` 以它的 `close`
  正常關閉 app。
- w2-059 已移除 UI 截圖比對與舊 CSV 驗證工具(`capture-window.ps1`、`image_diff.py`、`build-baseline.bat`、
  `seed_history_sqlite.py`、`compare_history_csv.py`、`verify_device_profile.ps1`):不再做 UI 自動化測試,
  歷史頁 / 匯出改由 5b / 5e 的 QTest 與 `docs\evidence\w2-041\tools\verify_export_csv.py` 驗證;
  `docs/evidence/` 下的歷史證據不變。
- 舊的 Python 開發網頁伺服器(8123)與它的 E2E 證據收集模式(頁面截圖 / console / Blob 下載上傳)
  已隨 w2-049 移除,網頁改由 desktop 的 `AppHttpServer` 提供;`docs/evidence/` 下的歷史證據不變。
  8123 自 w2-050 起改給 nginx 網頁前端;w2-057 起 nginx 預設改用 port 80(見「nginx 網頁前端與下載」)。

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
  `downloadPort = 8124`(w2-050 起可由 `TAIDAFLOW_DOWNLOAD_PORT` 設定,用 nginx 時設成 nginx 的 port(80),見「nginx
  網頁前端與下載」)。Core 不知道網頁用哪個主機名稱,所以只送路徑 + port;下載連結由網頁組成
  (`HistoryPage.qml` 的 `exportDownloadUrl()`):`url` 以 `/` 開頭 → `"http://" + Td.pageHost + ":" +
  downloadPort + url`(downloadPort 缺少時用 8124),例如 `http://192.168.0.125:8124/exports/<檔名>`;
  已是 `http...` 的完整網址則原樣使用。`Td.pageHost` 是 Proxy 的 **`STORED false`** 本機屬性(不進 Mirror
  contract),WASM `main.cpp` 設成與 mirror host 相同的 `location.hostname`(同一個 `127.0.0.1` 後備);
  desktop 保持空字串。「下載檔案」按鈕與完成時自動開啟都用這個網址。
  清理:每產生一個網頁檔後,若 `*.csv` 超過 20 個**或**總量超過 2 GB,依修改時間刪最舊的,直到兩個條件都
  滿足;不刪剛產生的檔,它單獨就超過 2 GB 時保留並記 log;正在被下載而刪不掉的檔記 log 跳過。
  啟動時刪除上次當機留下的暫存檔(`*.csv.XXXXXX`)。
- **下載**(w2-049 起為 `AppHttpServer` 單例的 `/exports` 掛載:`HistoryExportManager` 建立時掛上、
  shutdown 時卸下;listener `0.0.0.0:8124` 由 Core 啟停):只有 `GET`/`HEAD /exports/<檔名>`;檔名必須是
  `<sessionId>_<yyyyMMdd_HHmmss>.csv`(不能含 `/ \ .. :`、不能是其他副檔名),且實際路徑必須在匯出
  資料夾內(符號連結不跟隨);回應 `Content-Type: text/csv; charset=utf-8`、
  `Content-Disposition: attachment; filename="<檔名>"`、`Access-Control-Allow-Origin: *`、
  `Cache-Control: no-store`、`Content-Length`;檔案以 `QHttpServerResponder::write(QIODevice*)` 從磁碟
  分段送出(不整檔讀進記憶體)。不存在 404、壞檔名 400、其他方法 405。
  綁定失敗只記 warning,app 繼續執行(網頁匯出檔仍會寫出)。
  用 nginx 時(`TAIDAFLOW_DOWNLOAD_PORT=80`)下載改由 nginx 直接從匯出資料夾送(支援 Range 續傳),
  8124 的掛載保留作備援(不支援 Range)。
- 舊的 `Td.saveHistoryCsv()`(Q_INVOKABLE,前端組 10 筆 CSV)**已刪除**(main w2-042 自
  `TaidaFlowProxy.h` 移除,已合併進本分支);CSV 一律走上述匯出佇列。`docs/wasm-integration-report.md`
  中關於它的段落是歷史紀錄。

## 測試 / 驗證(全部以 exit code 判定)

QTest:`Core/AppHttpServer/tests`(可重用 HTTP 單例,只編該類別,見 5c)、5b 的歷史/匯出 harness 與
5e 的各連線端歷史檢視 harness;
其餘整合以下列可重跑檢查驗證:

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
:: 5. desktop 啟動(含安全探測、runtime-cwd、後端 + mirror 127.0.0.1:18125 + 區網轉發 0.0.0.0:8125
::    (同一 app PID)+ 8124 HTTP 服務起來、網頁 200、REST 127.0.0.1:18080 起來且 GET / 200(w2-060)、
::    關閉後 502/8124/8125/18125/18080 無殘留)
powershell -ExecutionPolicy Bypass -File scripts\verify-desktop-startup.ps1
::    (w2-049) 接模擬器:先 scripts\run-simulator.ps1,再加 -DeviceProfile simulator [-ProbeLog <檔案>]
:: 5a. (w2-043) 區網轉發:安全探測 SAFE 後啟動,以區網 IP:8125 與 127.0.0.1:8125 做 WebSocket upgrade
::     拿到 101(非白名單 Origin 也不被關閉)、區網 IP:18125 連不到;port 被占用時每 30 秒重查最多 30 分鐘
powershell -ExecutionPolicy Bypass -File docs\evidence\w2-043\tools\verify-lanrelay.ps1
:: 5b. (w2-041) 歷史區間 + CSV 匯出的 QTest(編譯真的 SqlManager / HistoryExport / Proxy 原始碼):
::     先做 30 天、兩個月份檔的測試資料(build\w2-041-bench),再建置並跑 CTest(需 8124 空著)。
::     w2-049 起下載測試改測 AppHttpServer 的 /exports 掛載,harness 在 docs\evidence\w2-049\tools
::     (w2-041 的原版留作歷史證據,已不能對現行原始碼編譯)
python -B docs\evidence\w2-041\tools\make_bench_db.py
docs\evidence\w2-049\tools\run-w2041-qtest.bat
::     30 天匯出檔逐列比對(獨立的 Python 實作)
python -B docs\evidence\w2-041\tools\verify_export_csv.py "build\w2-049-w2041-qtest\work\exports_30d\web-t30d_*.csv" --data build\w2-041-bench\data --from 1786896000 --to 1789487999
:: 5c. (w2-049) AppHttpServer 單例的獨立 QTest(靜態 200/304/gzip/MIME/HEAD/COOP-COEP、穿越攻擊、
::     下載掛載、自訂路由、綁定失敗、多執行緒註冊、1 GiB 串流記憶體;需約 2 GiB 暫存磁碟空間)
scripts\run-apphttpserver-tests.bat
:: 5d. (w2-050) nginx 網頁前端 + nginx 直送下載:部署 → 連結實驗(junction)與腳本規則(別人的 nginx 不動、
::     8123 被占用不啟動、含連結的資料夾拒絕)→ 模擬器 → 安全探測 SAFE → app(TAIDAFLOW_DOWNLOAD_PORT=8123)
::     → nginx-start → 127.0.0.1 與區網 IP 對 8123 的網頁 / 穿越 / /exports 檢查 → 經 Mirror 要求匯出
::     (與網頁相同流程,工具 build\w2-050-mirror-client)→ nginx 下載 SHA-256 相同、app log 無該筆請求 →
::     1 GB 檔的 Range / 中斷續傳 / If-Range / 記憶體 → 傳送中清理 → nginx-stop;再以未設 / 無效的
::     TAIDAFLOW_DOWNLOAD_PORT 啟動確認 downloadPort = 8124。需先建 mirror client 工具。
::     這支工具是以 port 8123 寫的;w2-057 起 nginx 預設 80,重跑前在同一個 shell 設 TAIDAFLOW_NGINX_PORT=8123
::     (nginx-web.ps1 沒給 -Port 時以它取代預設 80)。
docs\evidence\w2-050\tools\build-mirror-client.bat
set TAIDAFLOW_NGINX_PORT=8123
powershell -ExecutionPolicy Bypass -File docs\evidence\w2-050\tools\verify-nginx.ps1
set TAIDAFLOW_NGINX_PORT=
:: 5e. (w2-052) 各連線端獨立的歷史檢視 QTest(編譯真的 HistoryViews / SqlManager / HistoryExport / Proxy):
::     tst_w2052_views(多端交錯請求 = 單端結果、互不作廢、revision 只變自己的、閒置移除、上限 32、
::     不限區間、非法輸入)與 tst_w2052_rangepage(w2-045 正確性測試改走每端 API + 各端錨點)。
::     需 5b 的測試資料;不用任何 port。
docs\evidence\w2-052\tools\run-qtest.bat
::     app 執行中以 Mirror 客戶端送兩個 session 的請求並印出 historyViews(與網頁相同的路徑)
docs\evidence\w2-052\tools\build-mirror-client.bat
build\w2-052-mirror-client\mirror_history_client.exe --round "web-c1|month|1;web-c2|week|1" --round "web-c1|month|2"
::     (同一 round 內同一端的前一個請求視為會被後一個取代;--start-at-ms 讓多個客戶端同時開始送;
::      每筆 entry 印出整頁 records 的 SHA-1。w2-052 的實測輸出與檢查腳本見 docs\evidence\w2-052\)
:: 5f. (w2-057) 正式機打包資料夾:打包(exit 0)→ DLL 相依(exit 0)→ 自動啟動乾跑(-WhatIf,不註冊)→
::     實機檢查(安全探測 SAFE → 無 Qt 的 PATH 啟動 → nginx port 80 / 8124 / 8125 → 匯出 downloadPort=80、
::     Range 206 → 停止無殘留)。-SeedDb 是測試資料(開發機沒有設備,歷史資料表是空的)。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-release.ps1 -IncludeNginx -Force
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-package-deps.ps1 -Package dist\TaidaFlow-<日期>-<雜湊>
powershell -NoProfile -ExecutionPolicy Bypass -File dist\TaidaFlow-<日期>-<雜湊>\register-autostart.ps1 -WhatIf -UseNginx
docs\evidence\w2-050\tools\build-mirror-client.bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<日期>-<雜湊> -SeedDb build\runtime-cwd\data\sensor_202609.sqlite -FromMs 1790577423000 -ToMs 1790581023000
:: 5g. (w2-060) REST API:log 的 route 表與 RESTManager 實際註冊的一致(不用啟動 app);
::     app(simulator profile)+ nginx 執行中:每個 GET route 經 127.0.0.1 與區網 IP 的 nginx 80、OPTIONS 預檢、
::     PUT 三項寫測試值 → 讀回 → 寫回原值(只接受 build\ 下的 settings.sqlite,寫入在該檔驗證)、區網 IP 直連 18080 被拒
python -B scripts\check_rest_routes.py
python -B docs\evidence\w2-060\tools\rest_api_check.py --hosts 127.0.0.1,<區網 IP> --settings-db build\runtime-cwd\settings.sqlite
```

E2E 證據(雙向同步 double/bool、唯讀 PV 單向、斷線離線提示與控制項停用、重連恢復、
中文顯示)在 `docs/evidence/wasm-v4/`,檔名即步驟說明。CSV 匯出(desktop 存檔、網頁下載、
離線匯出、內容比對)在 `docs/evidence/wasm-v4-csv/`。
