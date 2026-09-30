# TaidaFlow WebAssembly 整合報告

現況架構、部署、問題報告與維護方建議

| 項目 | 內容 |
|---|---|
| 專案 | TaidaFlow(https://github.com/EllieFeng8/TaidaFlow.git) |
| 現況基準 | 分支 `core`,commit `58c6037`(合併 `main` 8a95a26),2026-09-27;歷史檢視部分為 `eec628f`(合併 `main` 8e3ea06)+ w2-052 |
| 初版整合基準 | 分支 `core`,commit `dc91f01`(「更新History」),2026-09-24(見 §2.6、§3.2 歷史紀錄) |
| 使用套件 | wasm-mirror integration pack **1.0.1**(wire protocol 3),`integration-pack/wasm-mirror/` 原封不動 |
| 建置環境 | Qt 6.8.3(msvc2022_64 / wasm_singlethread)、emsdk 3.1.56、MSVC 2022(VS 18 Community) |
| 網頁前端 | nginx 1.30.5(Windows 版,另一個程式;w2-062 起為正式前端並隨包附上)+ desktop 內建 `AppHttpServer`(8124,nginx 未啟用時的備援) |
| 文件最後對照 | core `0113799`(2026-09-29,w2-069:與最終實作逐節對照,見更新紀錄) |

**更新紀錄**

| 日期 | 內容 |
|---|---|
| 2026-09-24 | 初版(w2-027~w2-030):core dc91f01 + pack 1.0.1 整合、斷線鎖定、網頁 CSV 下載、接模擬器 |
| 2026-09-27 | 依現況更新(w2-054):網頁改由 desktop 內建 HTTP 服務與 nginx 提供、區網連線、歷史區間查詢與匯出、字型子集流程、建議逐條標狀態。過時段落移到「歷史紀錄」小節 |
| 2026-09-28 | w2-060:REST API(`RESTManager`)啟用,`127.0.0.1:18080`,區網經 nginx 80 的 `/api/`(§2.7);移除測試專用 PV 注入與 pack 1.0.0 的 zip 備份 |
| 2026-09-28 | w2-062:所有現場設定改由 `config.json`(設備位址、各服務 port、nginx、資料資料夾);網頁同步 port 由同源 `/runtime.json` 取得,nginx 啟用時網頁同步也經 nginx 80 的 `/mirror`(正式機防火牆只開 80 與 502);nginx 隨包附上並以 `start nginx` 啟動;檢查腳本改為 PowerShell;網頁版 QML 匯入掃描只看專案資料夾(§2.8) |
| 2026-09-29 | w2-069 依最終實作(core 0113799)修正:程式自寫 log 與 `nginx.exe` / `log.*` 鍵(w2-064 / w2-065,§2.8)、nginx 樣板改為六個值(加 log 資料夾);網頁載入頁轉場(w1-066);關閉流程 `Core::shutdown`(w2-067,§2.7);LanRelay(8125)定位為 nginx 未啟用時的備援、不再等待 pack 1.0.2(Mango 決定,DV-12、G-9、建議 19);§4.3 第 4、5、14 列、§4.6、建議 29 更新為現況 |
| 2026-10-01 | w2-084:網頁斷線偵測與自動恢復(§2.9):Core 每秒寫 `serverHeartbeatMs`(w1-083 契約)、網頁 5 秒顯示連線中斷 / 15 秒自動重新整理、載入頁在程式當掉或載入失敗 10 秒後自動重新整理,兩者共用 sessionStorage 退避 |

---

## 1. 結論(現況;2026-09-29 對照 core 0113799)

`core` 分支同一份程式可編成桌面版(authoritative,含全部後端)與網頁版(WebAssembly replica,只有
UI 與 `TaidaFlowProxy`)。兩端透過 wasm-mirror 同步 `Td` 的全部同步屬性,修改會互相看到。

- **設定檔 config.json**(w2-061 讀取器、w2-062 後端 / 腳本 / nginx 接上):設備位址、MS300 參數、Modbus 伺服器、HTTP、REST、
  mirror、nginx 與資料資料夾都在 `config.json`;程式缺檔時自動建立預設檔,JSON 錯誤時停止且不覆寫(§2.8)。
- **REST API 已啟用**(w2-060,Mango 2026-09-28):原後端的 `RESTManager` 由 Core 啟動,只綁 `127.0.0.1:18080`
  (config.json `rest`),區網經 nginx `http://<IP>/api/...`;GET 查詢 + PUT 改設定,**內網不做存取控管**(§2.7)。
- **後端只在桌面版**:Modbus、MS300、REST、SQLite、歷史匯出、HTTP 服務只編進桌面版
  (`scripts/check-wasm-backend.ps1` 檢查網頁版的後端來源、Qt 模組、字串皆為 0)。網頁版無法直接連任何設備。
- **網頁經 nginx(port 80)**:nginx 送網頁與 `runtime.json`、把網頁同步 `/mirror` 與 REST `/api/` 轉給 desktop、直接從匯出資料夾送
  CSV(支援續傳);desktop 內建 `AppHttpServer`(8124)與 LanRelay(8125)保留作 nginx 沒開時的備援。
  正式機部署(打包資料夾、隨包 nginx、啟動/停止腳本、自動啟動、防火牆 80 + 502)見 `docs/DEPLOY_AND_STARTUP.md`。
- **區網電腦可連線**:網頁先讀同源的 `/runtime.json` 得到同步 port(nginx 啟用時 = nginx 的 port,否則 8125),再連回
  「載入頁面的那台主機」;desktop 的 Mirror 綁在 `127.0.0.1:18125`(正式同步經 nginx 80 的 `/mirror` 轉到它;
  `App/lanrelay.h` 的 `0.0.0.0:8125` 只是 nginx 未啟用時的備援,因 pack 1.0.1 只允許 Mirror 綁 loopback 而保留;
  Mango 決定正式機防火牆只開 80 與 502,同步經 nginx,**不再等待 pack 1.0.2**)。依 Mango 決定為內網系統,**不做存取控管**(`allowedOrigins = {}`)。
- **斷線時網頁鎖住操作**:網頁顯示紅色「離線」橫幅(把頁面內容往下推,不遮擋),所有會送指令的控制項
  停用,重連後自動恢復(§4.2)。
- **歷史資料**:可跨月的時間區間查詢(每次推送 10 筆)與原始資料匯出(佇列、進度、取消、網頁下載 /
  桌面另存新檔)。**各連線端獨立的區間與頁碼**(spec §2.1,2026-09-27 Mango 修訂)**已完成**:main 端
  w1-052(8e3ea06,已合併進 core eec628f)+ Core 端 w2-052(§2.3);一端篩選或翻頁,其他端畫面不變。
- **pack 原封不動**:1.0.1 整包,`scripts/verify-pack.ps1` 驗 MANIFEST 24/24、25 檔與官方來源逐位元相同。
- **維運(2026-09-28~29)**:desktop 自己寫 log 檔(quiet `taidaflow-YYYY-MM-DD.log` 保留 60 天 + full `-full.log` 保留 7 天,
  config.json `log`,w2-064 / w2-065);關閉時 `Core::shutdown` 在 application 還在時依序停止後端,連著設備關閉也 exit 0
  (w2-067,§2.7);網頁載入頁在 App 畫面畫出後約 800 ms 轉場進入主畫面(w1-066,README「網頁載入畫面」)。

---

## 2. 架構與施作

### 2.1 現況架構與連接埠

```
 瀏覽器(區網任一台)                            desktop(Windows,TaidaFlowApp.exe,工作目錄 = config.json dataDir)
 ┌─────────────────────────┐  HTTP / WS 80  ┌──────────────────────┐   ┌──────────────────────────────────────────────┐
 │ TaidaFlowApp.html/.js/  │───────────────▶│ nginx 0.0.0.0:80     │   │ Mirror 127.0.0.1:18125(/mirror,protocol 3)  │
 │ .wasm(replica)         │                │ 網頁 <exe>\web、     │──▶│ REST 127.0.0.1:18080                         │
 │ 先 GET /runtime.json    │◀──────────────▶│ /runtime.json、      │   │ Core:Modbus 5×ADAM、Modbus server :502、   │
 │ Td(TaidaFlowProxy)     │                │ /mirror → 18125、    │   │       MS300、SqlManager(data\)、匯出      │
 └─────────────────────────┘                │ /api/ → 18080、      │   │ 備援:AppHttpServer 0.0.0.0:8124(網頁、   │
                                            │ /exports/ 讀匯出資料夾│   │       /exports)、LanRelay 0.0.0.0:8125     │
                                            └──────────────────────┘   └──────────────────────────────────────────────┘
```

| Port(config.json 預設) | 綁定 | 由誰提供 | 用途 |
|---|---|---|---|
| 80(`nginx.port`) | `0.0.0.0:80` | nginx(另一個程式;正式機隨包在 `<安裝資料夾>\nginx`,`start nginx`) | 網頁 `http://<IP>/`(→ 302 `/TaidaFlowApp.html`)、`/runtime.json`(no-store)、網頁同步 `/mirror`(WebSocket → 18125)、`/exports/<檔名>`(直接從匯出資料夾送,支援 Range)、`/api/...`(→ 18080) |
| 18080(`rest`) | `127.0.0.1:18080` | desktop `RESTManager`(w2-060) | REST API 內部 port,只限本機;區網經 nginx `/api/`(§2.7) |
| 8124(`http`) | `0.0.0.0:8124` | desktop 內建 `AppHttpServer`(`Core/AppHttpServer/`) | 備援:網頁 `http://<IP>:8124/TaidaFlowApp.html`、`/runtime.json`、`/exports/<檔名>`(不支援 Range) |
| 8125(`mirror.publicPort`) | `0.0.0.0:8125` | desktop `App/lanrelay.h` | 備援:nginx 沒開時網頁連的 mirror port,每條連線原樣轉發到 18125 |
| 18125(`mirror.internalPort`) | `127.0.0.1:18125` | desktop Mirror server(pack) | 內部 port,只限本機 |
| 502(`modbusServer`) | `0.0.0.0:502` | desktop `Modbus_Server`(聚合 server) | 既有功能,外部 HMI 用,與網頁無關(§4.3-4) |

- 網頁版的 mirror host = `location.hostname`;port = 同源 `/runtime.json` 的 `mirrorPublicPort`(desktop 每次啟動依
  config.json 寫入網頁資料夾:`nginx.enabled` 為 true 時 = `nginx.port`,否則 = `mirror.publicPort`),3 秒內讀不到或內容無效
  → 8125 並在 console 記 warning;路徑 `/mirror`。從哪個位址開網頁,就連回同一個位址。
- 下載連結由網頁組成:`http://<Td.pageHost>:<downloadPort><url>`(`url` = `/exports/<檔名>`)。
  `downloadPort` = config.json 的 `nginx.enabled ? nginx.port : http.port`(預設 80,由 nginx 送檔;連結
  `http://<host>:80/exports/...` 與不帶 `:80` 等價;`TAIDAFLOW_DOWNLOAD_PORT` 可臨時覆寫)。8124 的 `/exports` 一直保留作備援。
- 8124/8125 綁定失敗只記 warning,desktop 照常執行(本機 UI 不受影響,只是備援網址連不上)。
- 正式機防火牆只開 80 與 502(Mango 決定);8124 / 8125 / 18125 / 18080 不對外。

### 2.2 部署與啟動步驟(現況)

**步驟 0:安全確認(每次在開發機啟動 desktop 前)**。一律用 `scripts/run-desktop.ps1` 啟動(設定檔預設
`deploy/dev/config.dev.json`,以 `TAIDAFLOW_CONFIG` 交給程式),它先用同一份設定檔跑 `scripts/safety_probe.ps1`:
對實際生效的設備位址只做 TCP connect(1.5 秒逾時、不送 Modbus;本機位址則要由 Adam60xxSimulator 在聽)、列本機序列埠、
檢查 config.json 的各 port。設備可達、設定的序列埠存在、Modbus 伺服器 port 已被占用 → 不啟動(exit 3);
`http` / `mirror` / `rest` 的 port 已被占用 → 不啟動(exit 4 `BUSY`,不關掉占用者);nginx 的 port 只列出供參考。
desktop 的工作目錄 = config.json 的 `dataDir`(開發設定 `build\runtime-cwd\`)。

**步驟 1:建置**(依序,不可並行)

```bat
scripts\build-desktop.bat [fresh]                  :: build\desktop\TaidaFlowApp.exe
scripts\build-wasm.bat wasm-release [fresh]        :: build\wasm-release\TaidaFlowApp.html/.js/.wasm
```

**步驟 2:部署網頁**:`powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1 [-Source build\wasm-release] [-ExeDir build\desktop] [-NoGzip]`
清空 `<exe 資料夾>\web` 後複製同一次 build 的網頁檔,並產生 `.gz`(修改時間與原檔相同),再依 config.json 寫 `runtime.json`。
desktop 找網頁資料夾的順序:環境變數 `TAIDAFLOW_WEB_DIR` → `<exe 資料夾>\web` → 開發預設
`<exe 資料夾>\..\wasm-release`(相對路徑,不含建置機路徑),第一個含 `TaidaFlowApp.html` 的就用;都找不到只記 warning。

**步驟 3:啟動 desktop**:`powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -Label "<說明>" [-Config <設定檔>]`。
下載連結與網頁同步的 port 由 config.json 決定(nginx 啟用 → nginx 的 port)。啟動 log 應有 `[Config] ...`(每一項的值與來源)、
`WASM Mirror endpoint: ws://127.0.0.1:18125/mirror`、`LAN relay listening: 0.0.0.0:8125 -> 127.0.0.1:18125`、
`[Web] runtime.json written: ...`、`[Web] HTTP service listening on 0.0.0.0:8124`。

**步驟 4:nginx**:開發機可用桌面版建置產生的 `build\desktop\nginx`(`cd` 進去 `start nginx`,BUILD.md §4.5),或
`scripts\nginx-start.ps1` / `nginx-stop.ps1`(`scripts\nginx-web.ps1` 的捷徑,獨立的 `build\nginx` 前綴)。設定由同一支產生程式
(`scripts\install-nginx-config.ps1` 的函式)依 config.json 填入樣板 `deploy/nginx/taidaflow.conf` 的六個值(網頁根目錄、匯出資料夾、
log 資料夾(w2-065)、nginx port、REST port、Mirror 內部 port),`nginx -t` 通過才啟動。**先部署網頁(步驟 2)再啟動 desktop 與 nginx**:
nginx 的網頁根目錄是 `build\desktop\web`,desktop 也是啟動時才決定網頁資料夾。正式機:打包附 nginx,`install-nginx-config.ps1` 寫
`<安裝資料夾>\nginx\conf\nginx.conf`,以 `start nginx` 啟動(DEPLOY_AND_STARTUP.md §2)。

**步驟 5:防火牆**:正式機只放行 TCP 輸入 **80(nginx:網頁、同步、下載、REST)與 502(Modbus,不限來源)**;`netsh` 範例見
`docs/DEPLOY_AND_STARTUP.md` §7。由**管理員**設定;本專案的腳本不改防火牆 / 網路設定。

**接模擬器(測試用)**:先 `scripts\run-simulator.ps1`(Adam60xxSimulator 在 `127.0.0.201~205:502`),再
`scripts\run-desktop.ps1 -DeviceProfile simulator`(app 設 `TAIDAFLOW_DEVICE_PROFILE=simulator`,把 config.json 的位址換成
`127.0.0.201~205`)或 `-Config deploy\dev\config.simulator.json`。順序不能反(§4.6)。

### 2.3 歷史查詢與匯出(現況)

依 QtTester 的 `docs/taidaflow_history_export_spec.md`(PM 規格,不在 taidaflow repo 內)與 `README.md`「歷史資料:時間區間與匯出」。

**時間區間查詢(spec §2,core 現況)**
- 歷史頁起訖「日期與時間」選擇器(可輸入也可點選,分鐘精度;main 1af6884 / w1-047),「篩選」或 Enter 送
  `Td.historyViewRequested(clientSessionId, fromMs, toMs, 1)`;「顯示前一周」= 今天往前 7 天
  (main d752ded / w1-046)。Core 仍接受 `0 .. 8640000000000000` 的不限區間。
- Core 依請求的區間分頁,結果寫到該端的 `historyViews[sessionId]`,載入走 w2-039 的非同步 + 倒序分頁,
  每次只推送 10 筆。跨月:只讀實際存在且與區間相交的 `data\sensor_YYYYMM.sqlite`,各月 COUNT 有快取。
- w2-045(c321d6d):區間查詢在 SqlManager 執行緒上拆成約 6 ms 的短步驟,深頁與大區間不再長時間占住該執行緒
  (主執行緒每秒存檔要等它)。

**各連線端獨立的歷史檢視(spec §2.1,2026-09-27 修訂)——已完成(main w1-052 + Core w2-052)**
- 契約(main w1-052,8e3ea06,已合併進 core eec628f):同步屬性 `historyViews`(key = `clientSessionId`,
  值 `{fromMs, toMs, page, totalPages, totalRows, records, revision}`)+ 唯一請求
  `historyViewRequested(sessionId, fromMs, toMs, page)`;原本共享的 `historyRecords`、`historyCurrentPage`、
  `historyTotalPages`、`historyRangeFromMs/ToMs` 與 `historyRangeRequested` / `historyRefreshRequested` 已移除。
- Core(w2-052):`Core/HistoryViews.cpp` 的 `HistoryViewService` 依 sessionId 各自處理並寫整個 map;
  `revision` 為全域遞增、內容變才換;網頁端閒置 30 分鐘移除、最多 32 個、`desktop` 不移除。SqlManager 的
  過時判定與 keyset 錨點改為依 sessionId 分開,一端的請求不會讓另一端的請求作廢。細節見 `README.md`
  「歷史資料」與 `docs/evidence/w2-052/`。

**各連線端獨立的警報頁檢視(2026-09-30)——已完成(main w1-078 + Core w2-080)**
- 契約(main w1-078,`Core/TaidaFlowProxy.h` 的 `alarmViews` 區塊,Mango 核准):同步屬性 `alarmViews`(key =
  `clientSessionId`,值 `{fromMs, toMs, page, pageSize, totalCount, totalPages, activeCount, rows, state, message,
  revision}`)+ 唯一請求 `alarmViewRequested(sessionId, fromMs, toMs, page)`;`alarmRecords`(最近 3 天)不變,
  沒有自己的 entry 時警報頁照舊自己過濾 `alarmRecords`。
- Core(w2-080):`Core/AlarmViews.cpp` 的 `AlarmViewService` 依 sessionId 各自處理並寫整個 map,查詢在 SqlManager
  執行緒非同步分步執行(`Core/SqlManagerAlarms.cpp`,只讀實際存在的月份檔、每步最多 2000 筆);每頁 9 筆、新到舊
  (同一秒依 id)、`serialNumber` 為整個區間中的位置、`activeCount` 為區間內「未處理」筆數;`rows` 與 `alarmRecords`
  用同一個轉換函式(`Core/AlarmRecordFormat.cpp`)。警報寫入或解除後,重新查詢區間涵蓋該時間的 entry(保留區間與頁)。
  `revision` 內容變才換;網頁端閒置 30 分鐘移除、最多 32 個、`desktop` 不移除。細節見 `README.md`
  「警報頁:各連線端獨立的時間區間與分頁」與 `docs/evidence/w2-080/`。

**匯出(spec §3)**
- 觸發 `Td.historyExportRequested(sessionId, fromMs, toMs)`(區間 = 目前查詢區間),取消
  `Td.historyExportCancelRequested(sessionId)`。`sessionId` = `clientSessionId`(桌面 `desktop`,網頁分頁
  `web-xxxx`,`STORED false` 不同步)。
- `HistoryExportManager`:同時只跑一個、其餘 FIFO;同一 sessionId 已有排隊/執行中時拒絕新要求。
  狀態在同步屬性 `historyExportStatus[sessionId]`(`state`、`progress`、`queuePosition`、`rowsWritten`、
  `totalRows`、`fileName`、`url`、`downloadPort`、`savedPath`、`message`),進度節流(>= 1 個百分點且 >= 500 ms)。
- 生成在專用低優先權執行緒,用自己的唯讀 SQLite 連線、keyset 分段(每段 <= 2000 列)、邊讀邊寫
  `QSaveFile`,記憶體不隨檔案大小成長。CSV:UTF-8 BOM、CRLF、`序號` + 歷史頁 17 個欄位,數值換算與頁面相同。
- **桌面**:跳「下載歷史資料」另存新檔對話框,直接寫到選定位置。
- **網頁**:寫到 `<desktop 工作目錄>\exports\<sessionId>_<yyyyMMdd_HHmmss>.csv`,完成時送
  `url = /exports/<檔名>` 與 `downloadPort`,網頁組成完整網址並開啟下載。清理:`*.csv` 超過 20 個或
  總量超過 2 GB 時刪最舊的(不刪剛產生的檔;刪不掉的記 log 跳過)。
- **下載**:nginx(port 80,w2-057 前為 8123)(`^/exports/[A-Za-z0-9_-]{1,40}_\d{8}_\d{6}\.csv$`,支援 Range / If-Range,
  `max_ranges 1`)或 AppHttpServer 8124(同一檔名規則,串流、不支援 Range)。回應皆為
  `text/csv; charset=utf-8`、`Content-Disposition: attachment`、`Cache-Control: no-store`、
  `Access-Control-Allow-Origin: *`。
- 舊的 `Td.saveHistoryCsv()`(前端組 10 筆 CSV)已刪除(main w2-042,已合併),見 §4.4 歷史紀錄。

### 2.4 中文字型子集流程(現況)

Qt for WebAssembly 沒有系統 CJK 字型,因此內嵌 Noto Sans TC(OFL-1.1)子集:

1. 字型檔 `App/fonts/TaidaFlowNotoSansTC-{Regular,Bold}.ttf` 與 `App/fonts/charset.txt` **放在 git 裡**,建置直接使用;
   一般 clone、建置、部署都不需要任何額外工具。
2. 需要重新產生時(新增中文字串或新符號)用 `scripts\make-font-subset.ps1`(執行字型子集工具 `scripts\make_font_subset.py`,
   專案唯一的 Python 工具,需求與步驟見 `docs/BUILD.md` §2.5):掃描 `App/ Core/ TaidaFlow/ TaidaFlowContent/ Dependencies/`
   的非 ASCII 字元 + 可列印 ASCII + 常用全形標點,由 `C:\Windows\Fonts\NotoSansTC-VF.ttf` 實體化 wght 400/700 並子集化。
   `--check`:字元集變動或缺字時 exit 1,**每次合併 main 之後都要跑**。重產的兩個 TTF 與 `charset.txt` 與程式修改放在同一個 commit。
3. `App/embeddedfonts.cpp` 兩平台都 `addApplicationFont`,**只有網頁版**建 fallback 鏈;桌面顯示不變。

重產紀錄:w2-044(df6fa45,補「排隊中」等缺字)、w2-048(946753d,日期時間選擇器用字)、w2-055
(合併 8a95a26 後:加 `×` U+00D7、移除不再使用且字型沒有的 `✕` U+2715)。當時(w2-055)的紀錄:
513 字元(CJK 406),Regular 193,364 / Bold 194,036 bytes,各 897 glyphs,來源用字中字型缺少的字元 0 個。
這些數字只是歷史紀錄;目前的字元數、檔案大小與缺字一律以 `--check` 的輸出為準。

### 2.5 整合點與偏離清單(現況)

整合點原則上只有四個:**Proxy、CMake、main.cpp、HTTP server**。超出的步驟:

| 編號 | 內容 | 現況 |
|---|---|---|
| DV-1 | QML import 改為 `TaidaFlowBackend 1.0`(Td 移出 `Core` URI,避免與 `qt_add_qml_module(URI Core)` 撞名) | 沿用(main c7b7b9b 起 main 也相同) |
| DV-2 | 離線橫幅與控制項停用(TopNav、Main、MotorIcon、HistoryPage) | 沿用;橫幅已改為推開內容(c7b7b9b) |
| DV-3 | 中文字型子集(`App/embeddedfonts.*`、`App/fonts/`、`scripts/make-font-subset.ps1`) | 沿用,流程見 §2.4 |
| DV-4 | 測試專用 PV 注入(只編進桌面版,需環境變數才啟用) | **已移除**(w2-060;無設備時改用 Adam60xxSimulator,§4.6) |
| DV-5 | `scripts/` 建置、驗證、安全探測工具 | 沿用並擴充(deploy-web、nginx-*、run-apphttpserver-tests 等) |
| DV-6 | `.gitattributes`(保護 pack 位元組不被換行轉換) | 沿用 |
| DV-7 | README 轉 UTF-8 並補完整說明 | 沿用 |
| DV-8 | `CMakePresets.json`(`.gitignore` 仍排除,提交需 `git add -f`) | 沿用,見建議 17 |
| DV-9 | `docs/evidence/`(各任務證據) | 沿用 |
| DV-10 | `saveHistoryCsv` 網頁分支 | **已不適用**:`saveHistoryCsv` 已刪除,改為 Core 匯出 + 下載服務(§2.3、§4.4) |
| DV-11 | 測試用位址切換 `TAIDAFLOW_DEVICE_PROFILE=simulator` 與相關腳本 | 沿用(§4.6) |
| DV-12 | `App/lanrelay.h`:`0.0.0.0:8125` → `127.0.0.1:18125` 轉發(main e4bc327 / w2-042) | 沿用,定位為 nginx 未啟用時的備援(w2-062 起正式同步經 nginx 80 `/mirror`);不再等待 pack 1.0.2(建議 19) |
| DV-13 | `Core/AppHttpServer/`:可重用的 HTTP 單例,送網頁與下載(w2-049,5576b9e) | 取代舊的開發用網頁伺服器;w2-062 起為 nginx 的備援,另送 `runtime.json`(no-store) |
| DV-14 | `deploy/nginx/taidaflow.conf` + `scripts/nginx-*.ps1`(w2-050,b1ffaca) | w2-062 起是正式的網頁前端(隨包附上、`/mirror` 經 nginx),`TAIDAFLOW_DOWNLOAD_PORT` 只剩臨時覆寫 |
| DV-15 | REST API 啟用:`Core::startRestServer()`、`RESTManager::start()` 加綁定位址參數、nginx `/api/`(w2-060) | §2.7;w2-062 起位址 / port 來自 config.json,`TAIDAFLOW_REST_PORT` 移除 |
| DV-16 | `config.json`(w2-061 `App/appconfig.*` + w2-062 後端、腳本、`scripts/taidaflow-config.ps1`)與 `/runtime.json` | §2.8 |
| DV-17 | 根 `CMakeLists.txt`:網頁版 QML 匯入掃描只看專案資料夾(接手 Qt 內部函式 `_qt_internal_scan_qml_imports`)、桌面版產生 `build\desktop\nginx`(w2-062) | §2.8 |

### 2.6 歷史紀錄:2026-09-24 初版整合歷程與步驟

> 本小節保留 2026-09-24 初版(core dc91f01)的施作紀錄。其中「網頁伺服器」一步已被取代,取代做法見 §2.1、§2.2。

| 輪次 | 基準 | pack | 結果 |
|---|---|---|---|
| 第一輪 | `main` d2852fe | 1.0.0(repo 內附) | 通過,但發現 pack 1.0.0 的問題(§4.1),保存於分支 `backup/wasm-v2-pack1.0.0`,不採用 |
| 第二輪 | `main` d2852fe → 306dc1d | 1.0.1 | 依指示改拉最新版,中途暫停作廢 |
| 初版 | `core` dc91f01 | 1.0.1 | 通過(65b019a、68ed8b7、7d06ef1) |

初版步驟摘要:
- 步驟 0 安全確認:每次啟動 desktop 前跑 `safety_probe.ps1`;初版共啟動 9 次,皆 SAFE(`docs/evidence/wasm-v4/safety-probe.log`)。
- 步驟 1 放入 pack 1.0.1,以 pack 驗證腳本(現為 `verify-pack.ps1`)驗證;新增 `.gitattributes`。
- 步驟 2 CMake:先判斷 `EMSCRIPTEN` 再判斷 `WIN32`;SerialBus、SerialPort、Sql、HttpServer、Concurrent 只在桌面版;
  Proxy 註冊放在擁有 Proxy 標頭的 `Core` target。
- 步驟 3 main.cpp:桌面 `Core::instance().init()`;網頁直接建立 `TaidaFlowProxy` 當鏡像;`Td` 改註冊到 `TaidaFlowBackend`。
- 步驟 4 Proxy:新增 `transportReady`、`transportMessage`(`STORED false`)。
- 步驟 5 網頁伺服器(**已取代**):當時由一支開發用網頁伺服器腳本在 127.0.0.1:8123 送網頁(帶 COOP/COEP),
  mirror 是 `ws://127.0.0.1:8125`,並以 `allowedOrigins` 白名單只允許 8123 的頁面。
  取代它的做法:2026-09-24 w2-042/w2-043 起 mirror 改為 `127.0.0.1:18125` + LanRelay `0.0.0.0:8125`、
  `allowedOrigins` 不限制;2026-09-26 w2-049 刪除那支腳本,網頁改由 desktop 的 `AppHttpServer`(8124)提供;
  2026-09-27 w2-050 起 8123 改給 nginx;2026-09-28 w2-062 起網頁同步也經 nginx 80 的 `/mirror`。
- 步驟 6 QML 離線鎖定;步驟 7 中文字型子集(當時 490 字、中文 385 字,共 370 KB;現況見 §2.4)。

### 2.7 REST API(w2-060,現況)

- 原後端開發者的 `Core/RESTManager.{h,cpp}`(QHttpServer)已編譯多時但 Core 從未建立;w2-060 起 `Core::init()` 在
  `startHttpServer()` 後呼叫 `startRestServer()`:`RESTManager::start(port, address)`,w2-062 起位址與 port = config.json 的
  `rest.bind` / `rest.port`(預設 `127.0.0.1:18080`;環境變數 `TAIDAFLOW_REST_PORT` 已不讀)。綁定失敗只記
  `[REST] REST API NOT started ...`,app 照常;關閉時由 `Core::shutdown`(`aboutToQuit`,w2-067)在停止設備連線與 Modbus 伺服器
  之後刪除(關閉 listener),接著是歷史檢視、CSV 匯出、`AppHttpServer`,最後 SqlManager(README「關閉流程」)。
- w2-060 時 RESTManager 的改動只有一處:`start()` 多一個綁定位址參數(預設 `QHostAddress::Any` = 原行為);route 不變。
  w2-071 起 6 個 range 路由的回應改為分頁(見下表與表後說明),其他路由的回應不變。
- nginx `location ^~ /api/` → `proxy_pass http://127.0.0.1:<rest.port>`(HTTP/1.1、方法/body/query 原樣、連線 5 s、讀取 120 s、
  body 上限 1 MB、`Cache-Control: no-store`);`location = /api/` → REST 的 `/`(狀態)。CORS 標頭由 RESTManager 自己加(含 OPTIONS 預檢)。
- 開放範圍(Mango 決定):全部,讀 + 改設定,**內網不做存取控管**。

| 方法 | 路徑 | 用途 | 參數 | 會改到什麼 |
|---|---|---|---|---|
| GET | `/api/`(REST `/`) | 狀態 | — | — |
| GET / PUT | `/api/settings/sensors` | 感測器 key/名稱 | PUT body `[{"key","name"}]`(合併) | PUT:`settings.sqlite` `sensor_config`(app 其他地方不讀) |
| GET / PUT | `/api/settings/frequency` | `read_frequency` | PUT body `{"read_frequency":n}`,n > 0 | PUT:`settings.sqlite` `app_settings`(沒有程式使用) |
| GET / PUT | `/api/modbus/mode` | `network` / `standalone` | PUT body `{"mode":...}` | PUT:RESTManager 記憶體(signal 未連接,不影響 Modbus) |
| GET | `/api/sensor/range`、`/api/holding/range` | 區間的列,**分頁**(w2-071):`{"page","pageSize","totalCount","totalPages","hasPreviousPage","hasNextPage","items":[...]}`,`items` 舊到新、可跨月 | `from`、`to` epoch 秒;選填 `page`(預設 1)、`pageSize`(預設 200,最多 1000) | — |
| GET | `/api/sensor/rangeDateTime`、`/api/holding/rangeDateTime` | 同上(分頁,格式相同) | `from`、`to` ISO 8601 或 epoch 秒;選填 `page`、`pageSize` | — |
| GET | `/api/sensor/rangeDateTimePage`、`/api/holding/rangeDateTimePage` | 同 `rangeDateTime`(保留舊名稱) | 同上 | — |
| GET | `/api/sensor/last`、`/api/holding/last` | 本月最新一列(無 → 404) | — | — |
| GET | `/api/device/sn` | 序號 | — | `device_info.ini` 不存在時建立(`sn000000`) |

完整說明(回應格式、錯誤碼、PUT 範例)見 `README.md`「REST API」與 `docs/DEPLOY_AND_STARTUP.md` §8;
`scripts\check-rest-routes.ps1` 確認 log 的 route 表與 `RESTManager.cpp` 一致。
區間查詢一律分頁(w2-071):6 個 range 路由格式相同;`page`、`pageSize` 要是正整數且不超過 2147483647(`pageSize`
大於 1000 時當成 1000),`from`、`to` 要在 `0` ~ `253402300799` 之間且 `to >= from`,否則 400、不查詢;要整個區間依
`hasNextPage` 逐頁取。與舊版不同:以前 `range`、`rangeDateTime` 一次回傳整個區間的陣列(不分頁),大區間會讓 UI 停住
並占大量記憶體(建議 26);現在已沒有不限筆數的查詢。
已知限制:RESTManager 仍在主執行緒執行,每個查詢 blocking 等 SqlManager 執行緒,但每次最多讀 1000 列;
`totalCount`(COUNT)仍涵蓋整個區間,區間很大時這一步仍要掃過整個區間。`holding_register` 目前沒有資料(Manager 不寫)。

### 2.8 config.json、/runtime.json 與建置調整(w2-062,現況)

- **config.json**(規格 QtTester `docs/taidaflow_config_spec.md`):main 的讀取器 `App/appconfig.*` 在 `main.cpp` 最早讀入並切換
  `dataDir`;core 的後端透過 `Core::init()` 取值:5 台 ADAM 的 host/port/unitId(`ModbusClient` 建構參數;
  `TAIDAFLOW_DEVICE_PROFILE=simulator` 仍把位址換成 `127.0.0.201~205` 並記 log)、MS300 序列埠參數(`Ms300FaultReader::Settings`)、
  Modbus 伺服器 bind/port/unitId、`AppHttpServer` 的 bind/port、REST bind/port、下載連結 port(`nginx.enabled ? nginx.port : http.port`)。
  後端類別本身不讀檔(值由 `core.cpp` 傳入),各自的單獨測試專案不需要讀取器。逾時 / 重試 / 輪詢間隔仍寫在程式裡。log 的
  `[Config] Core ...` 行列出每個後端用的值與來源。w2-064(main)/ w2-065(core)另加 `nginx.exe`(預設 `nginx\nginx.exe`,相對於
  config.json 所在資料夾)與 `log`(`dir` 預設 `logs` 相對於 `dataDir`、quiet 60 天、full 7 天;程式自己寫 log 檔,啟動腳本不再轉存輸出);
  腳本的讀取器 `scripts\taidaflow-config.ps1` 不再有自己的預設值,一律取自 `TaidaFlowApp.exe --write-default-config`。
  欄位與預設值以 `--write-default-config` 的實際輸出為準(DEPLOY_AND_STARTUP.md §6 已逐項對照,w2-069)。
- **/runtime.json**(規格 §3 修訂):不是路由,是網頁資料夾裡的靜態檔 `{"mirrorPublicPort":<port>,"version":1}`
  (`TaidaFlowRuntime::buildRuntimeJson`)。desktop 每次啟動寫入(內容不同才寫;寫不進去只記 warning),`deploy-web.ps1` 部署時也寫
  一份;nginx 與 `AppHttpServer`(新的 `StaticOptions::fileCacheControl`)都以 `Cache-Control: no-store` 送出。
  port:nginx 啟用時 = `nginx.port`(網頁經 nginx `/mirror` 同步,Mango 2026-09-28:防火牆只開 80),否則 = `mirror.publicPort`。
- **nginx**:樣板多了 `location = /mirror`(WebSocket,讀寫逾時 3600 秒、不緩衝)與 `location = /runtime.json`(no-store);
  六個值(w2-065 加上 log 資料夾:`nginx-error.log`、`nginx-access-YYYY-MM-DD.log` 寫到 config.json 的 `log.dir`)全部由 config.json 帶入,唯一的產生程式 `scripts\install-nginx-config.ps1`(網頁根目錄寫成相對於 nginx 資料夾的 `../web`,
  檔頭記錄產生程式版本、config.json 路徑 / 時間 / SHA-256、安裝資料夾;`start-taidaflow` 發現不符時自動重產)。正式機隨包附
  nginx 1.30.5,以 `start nginx` 啟動;桌面版建置也產生 `build\desktop\nginx`。
- **網頁版 QML 匯入掃描**:Qt 6.8 以 repo 最上層為唯一掃描根目錄,`build\`、`dist\` 裡的 QML 會讓網頁版多連結用不到的模組
  (w2-063 發現:有 `dist\` 的工作樹 34,103,893 bytes、59 個匯入)。根 `CMakeLists.txt` 改為只掃專案資料夾(不含 `build*`、`dist`、
  隱藏資料夾):w2-062 工作樹(有 `build\`、`dist\`)與同一份原始碼的乾淨複本建出的 `TaidaFlowApp.wasm` 同為 33,847,174 bytes、
  45 個匯入(證據 `docs/evidence/w2-062/13-d6-wasm-size-compare.txt`)。
- **無 Python**:檢查腳本改寫為 PowerShell(`check-wasm-backend.ps1`、`check-version-shadow.ps1`、`check-rest-routes.ps1`、
  `verify-pack.ps1`,新舊版對同一建置輸出逐行相同後才刪舊檔),`verify-desktop-startup.ps1` 以 `CloseMainWindow` 關程式;唯一例外是
  字型子集工具(§2.4)。

### 2.9 網頁斷線偵測與自動恢復(w1-083 / w2-084,現況)

起因(Mango 2026-10-01 現場):網頁「半開」連線——網路斷了但 WebSocket 沒有關閉事件,`transportReady` 仍為 `true`,數值停住、
控制沒反應、沒有離線橫幅,F5 才恢復。pack 的 transport 狀態偵測不到這種情況,因此加上應用層心跳:

- **契約(main w1-083)**:`TaidaFlowProxy` 新增鏡像屬性 `double serverHeartbeatMs`(預設 0 = 尚未收到)與
  `kServerHeartbeatIntervalMs = 1000`。新屬性改變 contract hash:網頁檔與桌面必須是同一次建置(正常部署本來如此)。
- **Core 寫心跳(core w2-084)**:`Core/ServerHeartbeat.*`,`Core::init()` 最後啟動,先立即寫一次,之後主執行緒 `QTimer` 每 1 秒寫
  目前 epoch ms;不經過 SqlManager;`Core::shutdown()` 第一步停止。只編進桌面版(`Core/CMakeLists.txt` 桌面區塊),網頁端只讀。
- **網頁偵測(main w1-083)**:`LinkWatchdog.qml/.js` 只在網頁執行,只看「值最後一次變化」的本機單調時間:5 秒沒有變化 →
  橫幅「連線中斷,正在恢復…」、所有操作停用;15 秒 → 自動重新整理(`App/main.cpp` 的 `WebPageControl`,`location.reload()`)。
  桌面真的關閉(WebSocket 關閉、`transportReady` false)時維持原本的離線橫幅,不重新整理。
- **載入頁當機自動重新整理(core w2-084)**:`App/wasm/TaidaFlowApp.shell.html` 在 WebAssembly 程式中止 / 結束(qtLoad 的 `onExit`)、
  未捕捉的 WebAssembly 執行錯誤(`RuntimeError`、Emscripten `Aborted(...)`)、下載失敗、不支援 WebAssembly 時,保留原本的紅字訊息,
  下一行倒數,10 秒後自動重新整理。其他腳本錯誤(瀏覽器外掛、跨網域的 `Script error.`)不處理。w1-066 轉場與減少動態效果不變。
- **退避(兩者共用,規則在 `LinkWatchdog.js`)**:`sessionStorage` 的 `taidaflow.autoReload.lastEpochMs` / `taidaflow.autoReload.streak`;
  第一次立即(心跳 15 秒 / 當機 10 秒後),之後與上一次至少間隔 60 / 120 / 240 秒,最多 5 分鐘;連線正常 60 秒後歸零。
  載入頁的函式以 node 測試逐一比對 `LinkWatchdog.js` 的結果(`App\wasm\tests\run-shell-tests.bat`)。`sessionStorage` 被封鎖時
  載入頁改為 60 秒後才重新整理(無法記住次數,避免每 10 秒重整一次)。
- 限制:偵測只在網頁看到第一次心跳變化後開始;分頁在背景被強力節流時,要回到前景才判定;多執行緒 WebAssembly 的 worker
  錯誤若沒有以 `RuntimeError` 形式傳到主頁面,不會觸發載入頁的自動重新整理(仍會由心跳偵測處理)。

---

## 3. 驗證結果

### 3.1 現行可重跑檢查

全部以 exit code 判定(細節與參數見 `README.md`「測試 / 驗證」):

| 檢查 | 指令 |
|---|---|
| pack 完整性 | `scripts\verify-pack.ps1 -Source ..\WebAssemblyTest\integration-pack\wasm-mirror`;`scripts\run-pack-tests.bat` |
| fresh 建置(依序) | `scripts\build-desktop.bat fresh` → `scripts\build-wasm.bat wasm-release fresh` |
| 網頁版不含後端 | `scripts\check-wasm-backend.ps1 build\desktop build\wasm-release` |
| VERSION 檔攔截 | `scripts\check-version-shadow.ps1 build\desktop build\wasm-release`(兩邊 0) |
| 字型涵蓋 | `scripts\make-font-subset.ps1 --check`(選用工具,§2.4) |
| desktop 啟動 | `scripts\verify-desktop-startup.ps1`(安全探測、mirror 18125 + relay 8125 同一 PID、8124 網頁 200、runtime.json no-store、關閉後無殘留) |
| 安全探測自我測試 | `scripts\probe_selftest_sim.ps1` |
| 歷史區間 + 匯出 QTest | `docs\evidence\w2-062\tools\make-bench-db.bat` → `docs\evidence\w2-049\tools\run-w2041-qtest.bat`、`docs\evidence\w2-045\tools\run-qtest.bat`、`docs\evidence\w2-052\tools\run-qtest.bat` |
| AppHttpServer QTest | `scripts\run-apphttpserver-tests.bat` |
| config.json / runtime.json / 程式 log 檔 QTest | `App/tests`(`tst_appconfig`、`tst_runtimeinfo`、`tst_applog`;`docs\evidence\w2-065\tools\run-app-tests.bat`) |
| 腳本的 config.json 讀取器、log 清理 | `docs\evidence\w2-065\tools\test-config-reader.ps1` |
| DI 警報跨重啟 QTest | `docs\evidence\w2-053\tools\run-qtest.bat` |
| 關閉流程(`SqlManager::shutdown`)QTest | `docs\evidence\w2-067\tools\run-qtest.bat` |
| 打包資料夾(nginx、/mirror、REST、下載、搬移、壞 JSON、log 檔) | `scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<...>` |
| REST route 表一致 | `scripts\check-rest-routes.ps1` |
| Core 單元測試(含伺服器心跳 `tst_server_heartbeat`) | `Core\tests\run-core-tests.bat fresh` |
| 載入頁當機自動重新整理(node,不開瀏覽器) | `App\wasm\tests\run-shell-tests.bat build\wasm-release\TaidaFlowApp.html` |

(`.ps1` 以 `powershell -NoProfile -ExecutionPolicy Bypass -File` 執行。)最近幾次實跑的結果見 `docs/evidence/w2-065/`、
`docs/evidence/w2-067/`、`docs/evidence/w2-067-fix1/`(`w2-062/` 是更早的一輪)。

### 3.2 歷史紀錄:2026-09-24 初版驗收結果

> 以下是 dc91f01 初版的驗收結果,由 PM 親自重跑。當時的網頁由 8123 的開發用網頁伺服器提供;之後的現況驗證見 §3.1。

| 項目 | 結果(2026-09-24) |
|---|---|
| pack 完整性 | MANIFEST 24/24;25 檔與官方來源逐位元相同 |
| pack 內附測試 | Engine、Runtime 兩組全部通過 |
| 桌面版 / 網頁版 fresh build | exit 0;`TaidaFlowApp.wasm` 33,657,326 bytes |
| 網頁版不含後端 | 7 個後端 `.cpp`、5 個後端 Qt 模組、後端字串皆為 0 |
| VERSION 檔攔截檢查 | 桌面 0、網頁 0 |
| 字型涵蓋 | 缺字 0 |
| 桌面外觀 | 主頁、警報、歷史三頁像素差異皆 0 |
| 雙向同步 | 數值(M1、M2、M3)與開關(緊急停止、二通閥)雙向皆通過 |
| 唯讀 PV | 桌面 → 網頁單向通過 |
| 斷線 / 重連 | 斷線後網頁顯示離線橫幅;約 5 秒自動重連並套用最新狀態 |
| 離線操作 | 離線時點緊急停止、M1、二通閥、復歸都沒有反應,重連後桌面確認未收到任何指令 |
| 設備安全 | 9 次啟動前探測皆 SAFE;沒有任何 Modbus 封包送達設備 |

當時的 PM 抽測也觀察到:測試中途本機 Wi‑Fi 從 192.168.0.x 換到 192.168.168.x,而 Core 每 3 秒會重連設備。
在可能連到真實設備的網路上,不應以「寫死真實 IP」的版本做開發測試(建議 8)。

---

## 4. 問題報告

### 4.1 pack 1.0.0 的問題(升級到 1.0.1 後已解決)

這些是第一輪用 repo 內附的 1.0.0 時發現的,也是改用 1.0.1 的原因。

1. **`VERSION` 檔會取代 C++ 標準標頭 `<version>`,而且不會報錯**:QDS 專案預設
   `CMAKE_INCLUDE_CURRENT_DIR ON`,Windows 檔名不分大小寫,Qt 標頭的 `#include <version>` 讀到 pack 根目錄的
   `VERSION`。在 d2852fe 重現,3 個 pack 編譯單元命中。1.0.1 已根治(改名 `VERSION.txt` 並內建防護);
   現況以 `check-version-shadow.ps1` 持續檢查(0/0)。
2. **MANIFEST 驗證失敗**:pack 內 `CMakeLists.txt` 被改過又改回,少一個空行。1.0.1 整包替換後 24/24。
3. **WASM 的 QML 模組 URI 撞名**:`"Core"` 與 `qt_add_qml_module(URI Core)`;`Td` 已移到 `TaidaFlowBackend`。
4. **WASM 沒有部分 Qt 模組**(Concurrent、SerialBus、SerialPort):只在桌面版引用。

### 4.2 安全問題:斷線時網頁仍可操作(已修正)

- 現象:第一輪沒有離線提示時,桌面關閉後網頁上的「緊急停止」仍可切成 ON,指令根本沒有送出。
- 修正:依 pack 文件 §9 實作離線提示,離線時停用所有會送指令的控制項。之後新增的歷史頁控制項
  (篩選、顯示前一周、下載 CSV、取消、上/下一頁)也綁定 `Td.transportReady`。
- 驗證:離線時點擊緊急停止等控制項都沒有反應;重連後桌面 log 確認沒有收到任何指令。

### 4.3 產品面觀察(逐項標示現況)

| # | 觀察 | 現況(2026-09-27;第 4、5、14 列 2026-09-29 更新) |
|---|---|---|
| 1 | 網頁端可以解除緊急停止(`emergencyStopSv` 是雙向同步屬性) | 仍存在(建議 1) |
| 2 | `saveHistoryCsv` 在網頁版於瀏覽器本機執行 | 已不適用:該函式已刪除,改為 Core 匯出(§2.3、§4.4) |
| 3 | 歷史頁區間與頁碼是共享狀態,一端翻頁其他端一起翻 | 已完成:改為各端獨立(main w1-052 8e3ea06 + Core w2-052,§2.3,建議 9) |
| 4 | Modbus server 綁 `0.0.0.0:502`(`Modbus_Server::start` 預設 `AnyIPv4`) | 可設定(config.json `modbusServer.bind`,w2-062);Mango 決定 502 對所有來源開放,預設仍 `0.0.0.0`(建議 6) |
| 5 | 設備位址寫死(ADAM 192.168.1.201~205、MS300 COM2) | 已完成:改由 config.json `devices`(w2-061 / w2-062);預設值仍是廠區位址,另有測試用 simulator profile(§4.6,建議 8) |
| 6 | 找不到 `data_schema.sql`,啟動 log 有 `Schema file not found`(SqlManager 以內建 schema 退回) | 仍存在(建議 10) |
| 7 | 沒有設備時 log 量很大(`request was not sent`、MS300 每秒重試) | 仍存在(建議 13) |
| 8 | Proxy 建構子填入示範資料(`initializeListData()`),網頁第一次同步前會顯示 | 仍存在(建議 12) |
| 9 | 部分 setter 沒有等值比較(`setMotorRunningSv/Pv`、清單屬性) | 部分改善:`historyTitle` 值不變不通知(main 9dea0d0);`setMotorRunningSv/Pv`、`setHistoryRecords` 等仍無比較(建議 11) |
| 10 | 每次啟動寫入一筆「設備啟動」警報 | 仍存在(行為未變) |
| 11 | `main` 與 `core` 分岔 | 已改為固定流程:UI 在 main、後端在 core,core 定期合併 main(31417f4、dbfe092、6585485、58c6037) |
| 12 | 歷史頁離線橫幅遮住內容 | 已修正:橫幅佔自己的一列、把內容往下推(c7b7b9b,`TopNav.qml` 註解) |
| 13 | Modbus 斷線期間的指令遺失、重連後不補送 | 仍存在;啟動時改為讀設備實際狀態同步 SV(3ada4b0 / w2-036),但執行中重連後仍不補送(建議 3、4) |
| 14 | DI 警報重啟後殘留「未處理」、仍異常時重複新增 | 已完成:w2-053(a292fe7)重啟後第一次讀值時接手或解除;QTest `docs\evidence\w2-053\tools\run-qtest.bat` |

第 13 項的重連機制(依程式碼):5 台 ADAM 每台獨立計時器,斷線或連線失敗後固定每 3 秒重試
(`kReconnectDelayMs = 3000`,無退避);MS300 每秒輪詢時順便重連;Modbus server 只在啟動時開一次。
未連線時的讀寫一律丟棄,只記 `Device is not connected; request was not sent.`,不排隊。

### 4.4 歷史紀錄:網頁版「下載 CSV」(w2-028,2026-09-24)

> 本小節是 2026-09-24 的做法,已被取代。取代它的做法:Core 匯出佇列 + 下載服務(w2-040 / w2-041 起,
> 下載由 AppHttpServer 8124 或 nginx(port 80)提供,見 §2.3)。`saveHistoryCsv` 已由 main w2-042 自
> `TaidaFlowProxy.h` 刪除並合併進 core。

當時 `saveHistoryCsv` 在網頁版(`Q_OS_WASM`)改用 `QFileDialog::saveFileContent` 由瀏覽器下載目前頁面的
資料(UTF-8 BOM,檔名 `TaidaFlow_History_yyyyMMdd_HHmmss.csv`),桌面版維持另存新檔。Mango 於 2026-09-24
在實際瀏覽器確認可用。它只能匯出前端手上的 10 筆,也就是改為 Core 匯出的原因之一。

### 4.5 pack 文件的落差(給 pack 維護方)

| 編號 | 內容 |
|---|---|
| G-1 | 文件暗示 HttpServer、Sql 也要移到桌面分支;實際上 wasm_singlethread 有這兩個,缺的是 SerialBus、SerialPort、Concurrent。WASM 的 CMakeCache 會出現 `Qt6Sql_DIR`(Qt 自己的 QML plugin 掃描),並沒有連結 |
| G-2 | 沒說明當 Proxy 標頭屬於另一個 STATIC QML module 時,註冊應放在哪個 target。本案放在擁有標頭的 `Core` target |
| G-3 | 每次重連失敗,`transportStateHandler` 會連續被呼叫兩次;從沒連上過也顯示「connection was lost」;訊息寫死英文 |
| G-4 | §9 範例沒說明要先更新 ready 和 message,再發通知 |
| G-5 | 沒提醒 replica 建構子若填有示範資料,網頁在第一次同步前會把它當真資料顯示 |
| G-6 | 沒提 `Q_INVOKABLE` 不會被轉送,會在網頁本機執行 |
| G-7 | 路徑範例不一致:§5.1、§7 用 `/mirror`,guide §10 用 `/`(本案用 `/mirror`) |
| G-8 | 只要求驗證 MANIFEST,沒提醒 `core.autocrlf=true` 會在 checkout 時破壞 hash;建議附 `.gitattributes` 範本 |
| G-9 | 1.0.0/1.0.1 只允許 Mirror 綁 loopback,區網連線只能靠宿主自行轉發。本案正式同步改經 nginx 80 的 `/mirror`(反向代理到 loopback),`App/lanrelay.h` 只作 nginx 未啟用時的備援,因此不需要等 1.0.2;若日後提供綁非 loopback 的正式開關,可再評估移除 LanRelay(建議 19) |

### 4.6 接 Adam60xxSimulator 聯調(測試用位址切換,w2-029 / w2-030)

- `TAIDAFLOW_DEVICE_PROFILE=simulator` → 五台 ADAM 改連 `127.0.0.201~205`(port、unit 用 config.json 的);未設定 → config.json
  `devices` 的位址(預設 `192.168.1.201~205`);其他值 → 記警告並維持 config.json 的位址。切換在 `Core/Modbus_Client.cpp`。
  w2-062 起另有 `deploy/dev/config.simulator.json`(位址直接是 `127.0.0.201~205`,`run-desktop.ps1 -Config` 指定)。
- 腳本:`scripts/run-simulator.ps1`、`safety_probe.ps1 / run-desktop.ps1 -DeviceProfile simulator`
  (502 只接受 `Adam60xxSimulator.exe` 在實際生效的設備本機位址上的 listener,五個端點都要在聽,否則 exit 5)。
- **順序一定是模擬器先**:app 先起時,它對 `127.0.0.20x:502` 的 ADAM 連線會被 app 自己的 `0.0.0.0:502` 接走
  (`docs/evidence/wasm-v4-sim/14-bind-order-core-first.txt`)。
- 初版觀察到的點位落差(模擬器 DI0 是「漏液」且預設 0、補水泵 coil)已由 w2-030 讓模擬器對齊 core
  (DI0 相位正常預設 1、DI1 漏液、DI2 補水泵 OL、DO3 補水泵;證據 92b0853)。
- 模擬器只在值改變時記 `Core → …`;寫入同值以 Core log 的 `[Modbus][Write completed]` 為準。
- 模擬器的 DI 只能在其視窗勾選(沒有 Modbus 寫入或命令列介面)。w2-053 改用 Windows UI Automation
  (`docs/evidence/w2-053/tools/sim_di.ps1`),不移動滑鼠、不操作 TaidaFlow 視窗。

### 4.7 本輪(2026-09-24~27)新發現

1. **Windows 版 nginx 沒有 `disable_symlinks`**(1.30.5 `nginx -t` 回 `unknown directive`),會跟隨網頁資料夾或
   匯出資料夾裡的 junction。現行防護:`install-nginx-config.ps1`、`nginx-start.ps1` / `reload`、`deploy-web.ps1` 掃描 reparse point,有就拒絕
   (exit 5);`/exports` 中名稱像匯出檔的資料夾一律 404。nginx 執行中才建立的連結掃不到(`docs/evidence/w2-050/live/`)。
2. **nginx 在 Windows 不是服務**,開機不會自己起來;本專案不註冊服務。
3. **8124 備援不支援續傳**(`Range` 被忽略,一律 200 整檔);續傳只有 nginx(port 80)提供。續傳的前提是檔案仍在匯出
   資料夾(清理規則可能已刪除,之後 404)。
4. **既有的阻塞式存檔對外部長讀取或鎖敏感**:主執行緒每秒 `saveSensorData`、警報寫入都以
   `BlockingQueuedConnection` 等 SqlManager 執行緒;SQLite 為 rollback journal(非 WAL),外部程式持有讀鎖或
   寫鎖時,寫入要等 SQLite busy timeout(未另外設定;實測約 5 秒)才失敗,這段時間主執行緒停住。w2-053 的 QTest 在鎖住
   月份檔時量到單次輪詢 5.5~7.4 秒(`docs/evidence/w2-053/10b-w2-053-qtest.utf8.log`)。w2-045 已讓歷史頁
   查詢拆成短步驟、匯出用自己的唯讀連線分段讀,但外部工具(例如對 live 資料庫的長查詢)仍會造成停頓。
5. **80(nginx)與 502 對區網開放且不做存取控管**(Mango 決定,內網系統;w2-062 起正式機防火牆只開這兩個,8124/8125 是
   不對外的備援);任何能連到 80 的人都能看畫面、送指令(含解除急停,建議 1)與下載匯出檔,並經 `/api/` 讀歷史資料、
   用 PUT 改 REST 設定(w2-060);任何能連到 502 的人都能讀寫 Modbus 伺服器的點位。

---

## 5. 維護方建議

每條標示狀態:**已完成**(附 commit / 任務)、**仍建議**、**進行中**、**已不適用**。編號沿用初版
(README 等文件引用「建議 8」),新增的從 19 起。

### 高(安全相關,上線前處理)

1. **決定網頁端能不能操作緊急停止,特別是「解除」**。——**仍建議**。目前任何能開網頁的人都能解除急停,
   而且網頁已可從區網連線(§4.7-5)。建議至少禁止網頁端解除急停,或加確認步驟與權限控管。
2. **保留離線鎖定,之後新增的控制項也要比照**(綁定 `Td.transportReady`)。——**仍建議(持續遵守)**。
   w2-040 / w1-046 / w1-047 新增的歷史頁控制項都已比照。
3. **Modbus 重連後補送目前設定值**。——**仍建議**。3ada4b0(w2-036)已改為啟動時讀設備實際狀態同步 SV,
   但執行中斷線重連後仍不補送、也不重新同步。
4. **緊急停止要確認送達**(失敗持續重送並在畫面警示;現場需有硬體急停迴路)。——**仍建議**。
5. **寫入失敗要讓操作員看得到**。——**仍建議**(目前只記 log)。
6. **Modbus server 不要綁 0.0.0.0**。——**可設定,維持開放**:w2-062 起 `config.json` 的 `modbusServer.bind` 可改;
   Mango 2026-09-28 決定 502 對所有來源開放(預設仍 `0.0.0.0`)。
7. 處理 `saveHistoryCsv` 在網頁版的行為。——**已不適用**:w2-028(68ed8b7)先完成網頁下載,之後
   `saveHistoryCsv` 由 main w2-042 刪除,改為 Core 匯出 + 下載服務(w2-040 / w2-041 / w2-049 / w2-050)。

### 中(正確性與維運)

8. **設備位址改成可設定**(ADAM IP、MS300 COM port)。——**已完成**(w2-061 讀取器 + w2-062 後端):5 台 ADAM 的
   host/port/unitId 與 MS300 的序列埠參數都在 `config.json`;`TAIDAFLOW_DEVICE_PROFILE=simulator` 仍保留作開發測試。
9. **決定歷史頁頁碼要不要共享**。——**已完成**:Mango 2026-09-27 決定各連線端獨立(spec §2.1);main 端
   w1-052(8e3ea06,已合併進 core eec628f)與 Core 端 w2-052 已完成(§2.3)。
10. **修正 `data_schema.sql` 找不到**(部署時一起帶,或改用 qrc 內嵌)。——**仍建議**。
11. **補上 setter 等值比較**(pack 文件 §6.1),並確認不影響重送指令的語意。——**仍建議(部分完成)**:
    main 9dea0d0 讓 `historyTitle` 值不變時不通知,main w1-052 的 `setHistoryViews` 也有比較(`setHistoryRecords`
    已隨共享屬性移除);`setMotorRunningSv/Pv` 等仍無比較。
12. **正式版移除 Proxy 建構子的示範資料**,或只在沒有 Core 時才填。——**仍建議**。
13. **重試加退避、降低 log 量**。——**仍建議**(ADAM 仍固定 3 秒)。
14. **調整離線橫幅位置,不要遮住內容**。——**已完成**(c7b7b9b,橫幅佔自己的一列、把內容往下推)。

### 低(維護整潔)

15. **不要修改 `integration-pack/wasm-mirror/` 裡的檔案**;升級時整包替換並跑 `verify-pack.ps1`。——**仍建議(持續遵守)**,現況仍是 1.0.1 原封不動。
16. **刪除 pack 1.0.0 的 zip 備份**(`integration-pack/` 下的舊版壓縮檔,目前用 1.0.1)。——**已完成**(w2-060)。
17. **`.gitignore` 不要再排除 `CMakePresets.json`**。——**仍建議**(`.gitignore` 仍有 `/CMakePresets.json`)。
18. **`main` 和 `core` 的分岔要找時間合併**。——**已完成(改為固定流程)**:UI 在 main、後端在 core,core 定期
    合併 main(31417f4、dbfe092、6585485、58c6037);每次合併後要重產字型子集(建議 23)。

### 新增(2026-09-27)

19. **wasm-mirror pack 1.0.2 提供 Mirror 綁非 loopback 的正式開關後,改由 Mirror 直接綁 `0.0.0.0:8125`,
    刪除 `App/lanrelay.h` 與 `main.cpp` 中的使用**。——**已不需要等待(2026-09-29 更新)**:Mango 決定正式機防火牆只開 80 與 502,
    網頁同步經 nginx 80 的 `/mirror`(w2-062);8125 的 LanRelay 只是 nginx 未啟用時的備援,pack 1.0.2 不是必要條件。
    若日後 pack 支援 Mirror 綁非 loopback,可再評估是否移除 LanRelay。
20. **Windows 版 nginx 沒有 `disable_symlinks`**:限制網頁資料夾與匯出資料夾的寫入權限(只讓部署帳號與 app
    寫入),或改用有 `disable_symlinks` 的平台;現行腳本只在啟動 / reload / 部署時掃描(§4.7-1)。——**仍建議**。
21. **降低既有阻塞式存檔對外部讀取的敏感度**:評估 SQLite WAL 模式或把每秒存檔改為非同步,並在操作手冊註明
    app 執行中不要對 live 資料庫做長查詢(§4.7-4)。——**仍建議**。
22. **nginx 不是 Windows 服務**:需要開機自動啟動時的做法。——**已完成**(w2-062):正式機以 nginx 標準方式 `start nginx`
    啟動,`start-taidaflow`(登入時的排程)在 nginx 沒執行時一併帶起;不做成服務。
23. **合併 main 後一律跑 `make-font-subset.ps1 --check`**(w2-044 就是合併後出現缺字「排□中」);可加入合併檢查清單。——**仍建議**。
24. **若網路不再是可信內網,重新評估 80 與 502 的存取控管**(來源限制、防火牆範圍、急停權限;含 80 的 REST `/api/`)。——**仍建議**。

### 新增(2026-09-28)

25. **RESTManager(原後端的 REST API)啟用與開放範圍**。——**已啟用**(w2-060):`127.0.0.1:18080` + nginx `/api/`,
    GET + PUT 全部開放、內網不做存取控管(Mango 2026-09-28 決定,§2.7)。
26. **REST 查詢不要卡住 UI**:當時不分頁的 `/api/sensor/range`、`/api/holding/range`、`rangeDateTime` 會一次讀完整個區間
    (主執行緒 blocking 等 SqlManager);建議限制區間 / 筆數,或把 RESTManager 移到自己的執行緒。——**已完成**(w2-071):
    6 個 range 路由全部分頁(`pageSize` 預設 200、最多 1000,`from`/`to`/`page`/`pageSize` 超界回 400,§2.7)。RESTManager
    仍在主執行緒 blocking 等 SqlManager,但每次最多讀 1000 列;移到自己的執行緒未做。
27. **REST 的 PUT 目前沒有實際作用對象**:`read_frequency`、`sensor_config` 沒有程式讀取,`modbusModeChanged` 沒有連接;
    若要讓它們生效需另案定義行為。——**仍建議(由維護方決定)**。
28. **網頁版 QML 匯入掃描的範圍限制依賴 Qt 6.8 的內部函式**(`_qt_internal_scan_qml_imports`,§2.8):升級 Qt 時確認
    configure log 仍有 `[qml-scan] QML import scan limited to the project folders`;若 Qt 之後提供正式參數,改用正式參數。——**仍建議**。
29. **`nginx.exe` 鍵補進程式的 config.json 讀取器**(當時只有腳本讀,程式把它當不認得的鍵忽略並記 log)。——**已完成**:
    main w2-064 把 `nginx.exe` 與 `log.*` 加進 `App/appconfig.*`(`--write-default-config` 會寫出),core w2-065 合併並讓腳本
    一律取程式的預設值(`scripts\taidaflow-config.ps1` 不再有自己的備援值)。

---

## 附錄 A:如何執行(現況)

```bat
scripts\build-desktop.bat                     :: 桌面版
scripts\build-wasm.bat wasm-release           :: 網頁版(桌面版建完再建)
```

1. 部署網頁:`powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1`(寫 `build\desktop\web`、`.gz` 與 `runtime.json`)
2. 啟動桌面版:`powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -Label "manual"`
   (設定檔 `deploy\dev\config.dev.json`;先安全探測,工作目錄 = 它的 dataDir `build\runtime-cwd`)
3. nginx:`cd build\desktop\nginx` 後 `start nginx`(桌面版建置產生;停止用 `nginx -s quit`),
   或 `scripts\nginx-start.ps1` / `scripts\nginx-stop.ps1`(隔離的 `build\nginx`)
4. 瀏覽器開 `http://127.0.0.1/`(區網:`http://<desktop 的區網 IP>/`)。不開 nginx 時用 `http://127.0.0.1:8124/TaidaFlowApp.html`,
   此時設定檔要 `nginx.enabled=false`,網頁才會連 8125 同步

接模擬器:先 `scripts\run-simulator.ps1`,再 `scripts\run-desktop.ps1 -DeviceProfile simulator`,網頁同上。
注意:`run-desktop.ps1` / `run-simulator.ps1` 的輸出不要接管線,也不要在 PowerShell 裡用 `>` 導向檔案,否則指令要等程式
結束才返回;直接執行,或在 cmd 以 `>` 導向檔案即可(`docs/evidence/w2-069/03-pipe-redirect-test.txt`)。

詳細說明見 `README.md`。

## 附錄 B:查證方式(2026-09-27 core 58c6037;2026-09-28 w2-062 更新)

| 內容 | 查證 |
|---|---|
| Mirror 18125 / relay 8125 / `allowedOrigins = {}` / `location.hostname` / `/runtime.json` | `git grep -n "allowedOrigins\|mirror()" -- App/main.cpp`、`App/lanrelay.h` 檔頭、`App/runtimeinfo.h` |
| config.json 讀取與後端接線 | `App/appconfig.h`;`git grep -n "AppConfig::instance\|deviceSettingsFromConfig\|writeRuntimeJson" -- Core/core.cpp` |
| 8124 HTTP 服務、網頁資料夾順序、runtime.json no-store | `git grep -n "resolveWebDir\|fileCacheControl" -- Core/core.cpp Core/AppHttpServer/AppHttpServer.h`、`Core/AppHttpServer/README.md` |
| 下載 port(`nginx.enabled ? nginx.port : http.port`) | `git grep -n "downloadPort" -- App/appconfig.cpp Core/core.cpp` |
| nginx(`/mirror`、`/runtime.json`、`/exports` 規則、無 `disable_symlinks`)與唯一的產生程式 | `deploy/nginx/taidaflow.conf`、`scripts/install-nginx-config.ps1`、`scripts/taidaflow-config.ps1` 的 `Get-TaidaFlowNginxConf` |
| 部署腳本參數 | `scripts/deploy-web.ps1`(`-Source`、`-ExeDir`、`-NoGzip`、`-Config` / `-NoConfig`) |
| `saveHistoryCsv` 已刪除 | `git grep -n saveHistoryCsv -- App Core TaidaFlowContent` 無結果 |
| 歷史頁各端獨立(w2-052 更新) | `git grep -n "historyViewRequested\|setHistoryViews" -- Core/HistoryViews.cpp Core/TaidaFlowProxy.h`(有);`git grep -n "historyCurrentPage\|historyRangeRequested\|historyRefreshRequested" -- Core App TaidaFlowContent` 無結果 |
| 離線橫幅推開內容 | `TaidaFlowContent/TopNav.qml` 的 `offlineBanner`(`height: visible ? 64 : 0`)與註解;`git log -- TaidaFlowContent/TopNav.qml` |
| Modbus server 位址(config.json `modbusServer`)、重連 3 秒 | `git grep -n "modbusServer()" -- Core/core.cpp`、`git grep -n kReconnectDelayMs -- Core/Modbus_Client.cpp` |
| 示範資料、setter 無比較 | `Core/TaidaFlowProxy.h`(建構子 `initializeListData()`、`setMotorRunningSv`) |
| pack 版本、舊版 zip 已刪、`.gitignore` | `integration-pack/wasm-mirror/VERSION.txt`、`ls integration-pack`(只剩 `wasm-mirror/`)、`git grep -n CMakePresets -- .gitignore` |
| REST API(w2-060、w2-062) | `git grep -n "startRestServer\|kRestRoutes\|rest()" -- Core/core.cpp`、`deploy/nginx/taidaflow.conf` 的 `location ^~ /api/`、`scripts\check-rest-routes.ps1` |
| 網頁版 QML 匯入掃描範圍(w2-062) | 根 `CMakeLists.txt` 最後一段、網頁版 configure log 的 `[qml-scan]` 行 |
| 字型數字 | `scripts\make-font-subset.ps1 --check`、`docs/evidence/w2-055/` |
| 舊伺服器檔名已不在本報告 | `git grep -n "serve_[w]asm" -- docs/wasm-integration-report.md` 無結果(正規式寫法避免本列自己被找到) |
