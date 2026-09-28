4. **設定只有一個檔:`C:\TaidaFlow\config.json`**(與 `TaidaFlowApp.exe` 同一層,w2-060)。資料夾位置
   (預設 `C:\TaidaFlowData`)、要不要 nginx、nginx port、REST 內部 port 都寫在這裡;app 自己與所有腳本都讀它
   (見 [§1.2](#12-設定檔-configjson))。資料都在 `C:\TaidaFlowData`,程式資料夾裡沒有資料,更新程式不會動到資料。

---

## 1. 正式機(工廠現場)

### 1.1 需要什麼

| 項目 | 說明 |
|---|---|
| Windows | Windows 10(1809 以上)或 Windows 11,**64 位元**(Qt 6.8 的支援範圍)。 |
| VC++ 執行環境 | **不用另外安裝**:打包資料夾已附 `vcruntime140*.dll`、`msvcp140*.dll` 等(與建置用的 MSVC 14.51 同版,app-local 方式)。若 IT 規定改用系統安裝的 VC++ 可轉散發套件,請安裝 **Visual C++ 2015-2022(x64)14.51 以上**(`vc_redist.x64.exe`,由管理員執行),打包資料夾內的同名 DLL 仍會優先使用。 |
| Qt | **不用安裝**。Qt 的 DLL、plugins、QML 模組都在打包資料夾裡。 |
| nginx | 用 `-IncludeNginx` 打包時已在 `nginx\` 資料夾(nginx 1.30.5,附授權檔 `nginx\docs\LICENSE`);否則放在 `C:\tools\nginx\nginx-<版本>\`。 |
| 中文字型 | 桌面畫面用 Windows 內建中文字型(微軟正黑體)。若畫面出現方框,見 [§5 常見問題](#5-常見問題)。 |
| 網路 | 本機要在 `192.168.1.x` 網段才連得到 ADAM(位址固定在程式內);`COM2` 接 MS300。 |

### 1.2 設定檔 config.json

**唯一的現場設定檔**是程式資料夾裡的 `C:\TaidaFlow\config.json`(與 `TaidaFlowApp.exe` 同一層,UTF-8 文字檔,
用記事本改即可)。打包資料夾附的內容(預設值):

```json
{
    "dataDir": "C:\\TaidaFlowData",
    "useNginx": true,
    "nginxPort": 80,
    "restPort": 18080
}
```

| 鍵 | 型別 | 預設(打包附的值) | 沒有這個鍵 / 沒有 config.json 時 | 誰讀 | 說明 |
|---|---|---|---|---|---|
| `dataDir` | 字串 | `"C:\\TaidaFlowData"` | app:不切換,用「目前的工作目錄」;腳本:`<程式資料夾>\runtime` | **app 本身**與全部腳本 | 資料夾 = app 的工作目錄(設定、資料庫、匯出、log)。app 啟動最早期(寫任何檔之前)讀它、建立資料夾並切過去,所以**雙擊 exe、`start-taidaflow`、開機自動啟動都寫到同一處**。相對路徑 = 相對於 config.json 所在資料夾 |
| `useNginx` | `true` / `false` | `true` | `false` | 腳本 | `start-taidaflow` 是否一起啟動 nginx(網頁、CSV 下載續傳、REST `/api/`) |
| `nginxPort` | 整數 1~65535 | `80` | `80` | 腳本 | nginx 的 port(`http://<IP>/`;改成例如 `8123` 時網址是 `http://<IP>:8123/`,防火牆也要改) |
| `restPort` | 整數 1~65535 | `18080` | `18080` | 腳本(傳給 app 與 nginx) | REST API 的內部 port(只綁 127.0.0.1,區網用 nginx 的 `/api/`,§1.11) |

- **路徑的反斜線要寫兩次**(JSON 規則):`"C:\\TaidaFlowData"`;也可以寫 `"C:/TaidaFlowData"`。寫成
  `"C:\TaidaFlowData"` 是錯的 JSON:腳本會回結果碼 `2` 並顯示原因,什麼都不啟動;直接雙擊 exe 時 app 在 log
  記一行 `[Config] ... is not a valid JSON object` 並照舊用目前資料夾。
- 以 `_` 開頭的鍵是註解(例如 `"_comment": "..."`),不理會;其他不認得的鍵:腳本與 app 都記一行「ignored」後忽略。
  鍵名大小寫要完全一樣。
- 型別錯(例如 `"nginxPort": "80"`)或路徑含 `| < > "` 等不能用的字元 → 腳本結果碼 `2`,什麼都不啟動。
- 資料夾不能建立或不能寫(例如不存在的磁碟 `Q:\...`):腳本結果碼 `2`;直接雙擊 exe 時 app 記
  `[Config] data folder "..." cannot be created` 警告,**照舊用目前資料夾,不會當掉**。
- app 的 log 一開始就會印出設定來源與實際資料夾,例如
  `[Config] data folder C:\TaidaFlowData (from C:\TaidaFlow\config.json; working directory was C:\TaidaFlow)`。
- **命令列參數優先**(只影響那一次):`start-taidaflow.bat -Port 8123`、
  `start-taidaflow.ps1 -DataDir D:\Test -UseNginx:$false -RestPort 18081`。腳本把它用的資料夾以環境變數
  `TAIDAFLOW_DATA_DIR` 交給 app(app 以它優先於 config.json),所以 app 一定寫到腳本用的那個資料夾。
- 不放在 config.json 的:`TaidaFlowSettings.ini`(警報門檻等)、`device_info.ini`(序號)、`settings.sqlite`
  維持原樣,都在資料夾裡(§1.3 的表)。
- 改了 config.json 之後:先 `stop-taidaflow.bat`(用**舊**設定找到執行中的 app 與 nginx)再改檔,改完再
  `start-taidaflow.bat`。app 執行中改 `dataDir` 會讓 stop 找不到它(結果碼 `1`,見 §1.5)。

### 1.3 安裝(第一次)

1. 在開發機打包(開發人員做,見 [§3.5](#35-打包正式機資料夾開發人員)):得到
   `dist\TaidaFlow-<日期>-<版本>\` 整個資料夾(含 `config.json`)。
2. 把**整個資料夾**複製到正式機並改名,建議固定放在 **`C:\TaidaFlow\`**(也就是
   `C:\TaidaFlow\TaidaFlowApp.exe`、`C:\TaidaFlow\config.json`、`C:\TaidaFlow\start-taidaflow.bat`;以下都用這個路徑。
   版本記在 `VERSION.txt`。不要放在 `C:\Program Files`,那裡一般使用者不能寫入)。
3. 需要時用記事本改 `C:\TaidaFlow\config.json`(§1.2),例如資料夾放 D 槽:`"dataDir": "D:\\TaidaFlowData"`。
   不改就是 `C:\TaidaFlowData`。資料夾不用先建立:第一次啟動時會自動建立並先試寫(不能建立或寫入 → 結果碼 `2`)。
   一般使用者帳號預設可以在 `C:\` 下建立資料夾;公司若有限制,請管理員先建立並給該使用者寫入權限。
4. 防火牆(由**管理員**設定,見 [§1.10](#110-防火牆由管理員設定))。
5. 第一次啟動(§1.4)後,資料夾(`dataDir`)裡會出現(資料夾本身由 app 或啟動腳本建立;`logs\`、`nginx\` 由啟動腳本建立,其餘由 app 建立):

   | 檔案 / 資料夾 | 內容 |
   |---|---|
   | `TaidaFlowSettings.ini` | 設定(`[Alarm] aiHighAlarmPercent` 等) |
   | `settings.sqlite` | 感測器名稱、讀取頻率等設定(REST API 的 PUT 會改這裡,見 §1.11) |
   | `device_info.ini` | 設備序號(`[device] sn`;REST API 啟動時沒有就建立,內容 `sn000000`) |
   | `data\sensor_YYYYMM.sqlite` | 歷史資料與警報,每月一個檔 |
   | `exports\` | 網頁匯出的 CSV(超過 20 個或 2 GB 時自動刪最舊的;啟動腳本的 nginx 也會先建立) |
   | `logs\` | `launcher.log`(啟動/停止紀錄)與每次啟動一個 `taidaflow-<日期-時間>.log` |
   | `nginx\` | nginx 的設定(`conf\taidaflow.conf`,自動產生)、log、暫存 |
   | `taidaflow-app.json` | 執行中的 app 身分(停止時用;停止後刪除) |

   (`settings_schema.sql` / `data_schema.sql` 不需要:找不到時 app 用內建結構,log 會有一行
   `Schema file not found`,屬正常。)

### 1.4 啟動:要開哪個檔

**雙擊 `C:\TaidaFlow\start-taidaflow.bat`。**

- 它會:讀 `config.json` → 檢查是否已在執行 / port 被占用(502、8124、8125、18125、`restPort`、`useNginx` 時的
  `nginxPort`)→ 啟動 nginx(`/api/` 轉給 `restPort`)→ 啟動 `TaidaFlowApp.exe`(工作目錄 = `dataDir`)
  → 確認 8124、8125 在聽(`restPort` 沒在聽只記警告)→ 顯示網址、資料夾與結果碼,按任意鍵關閉這個黑色視窗(app 會繼續執行)。
- 結果碼:`0` 成功;`4` 已經在執行或 port 被占用(**什麼都沒啟動**,也不會關掉別的程式);
  `6` app 沒起來(看資料夾 `logs\` 裡最新的 log);`8` app 已啟動但 nginx 沒起來(網頁改用 `:8124`);
  `2` 打包資料夾不完整、資料夾不能寫,或 `config.json` 有錯(畫面上會寫原因)。
- 直接雙擊 `TaidaFlowApp.exe` 也會用 `config.json` 的資料夾(app 自己讀),但不會啟動 nginx,網頁上的下載連結
  也會指到 app 自己的 `:8124`。平常請用 `start-taidaflow.bat`;手動做法見 §1.5。

### 1.5 不用任何腳本的手動做法(cmd)

打開「命令提示字元」(開始 → 輸入 `cmd`),**一行一行**貼上。先設定兩個路徑(`DATA` = config.json 的 `dataDir`):

```bat
set "TF=C:\TaidaFlow"
set "DATA=C:\TaidaFlowData"
```

**(a) 啟動 app**

```bat
set TAIDAFLOW_DEVICE_PROFILE=
set TAIDAFLOW_DATA_DIR=
set TAIDAFLOW_REST_PORT=18080
set QT_PLUGIN_PATH=
set QML_IMPORT_PATH=
set QML2_IMPORT_PATH=
set TAIDAFLOW_DOWNLOAD_PORT=80
start "" "%TF%\TaidaFlowApp.exe"
```

- 資料夾:app 自己讀 `%TF%\config.json` 的 `dataDir`,建立並切換過去(不需要先 `cd`)。
- `set TAIDAFLOW_DEVICE_PROFILE=`(等號後面空白)= 清除測試用的模擬器設定,正式機**一定**要清掉;
  `set TAIDAFLOW_DATA_DIR=` 清掉可能殘留的資料夾覆寫(它優先於 config.json)。
- `TAIDAFLOW_DOWNLOAD_PORT=80`:網頁上的 CSV 下載連結走 nginx(= `nginxPort`);不設就走 app 的 8124。
- `TAIDAFLOW_REST_PORT=18080`:REST API 的內部 port(= `restPort`;只綁 127.0.0.1;不設也是 18080)。要與 nginx 設定檔的
  `@TAIDAFLOW_REST_PORT@` 相同。
- 確認:`netstat -ano | findstr LISTENING | findstr ":8124 :8125 :502 :18080"` 要看到這四個 port(18080 是 `127.0.0.1:18080`);
  app 的資料夾出現 `settings.sqlite` 等檔(§1.3 的表)。

**(b) nginx 第一次:準備設定檔**(之後不用再做)

```bat
mkdir "%DATA%\nginx\conf" "%DATA%\nginx\logs" "%DATA%\nginx\temp" "%DATA%\exports"
copy "%TF%\deploy\nginx\taidaflow.conf" "%DATA%\nginx\conf\taidaflow.conf"
notepad "%DATA%\nginx\conf\taidaflow.conf"
```

在記事本用「取代」(Ctrl+H)換掉四個記號,路徑一律用**正斜線 `/`**,存檔(值來自 config.json):

| 記號 | 換成(以本節路徑為例) |
|---|---|
| `@TAIDAFLOW_WEB_ROOT@` | `C:/TaidaFlow/web` |
| `@TAIDAFLOW_EXPORT_DIR@` | `C:/TaidaFlowData/exports`(= `dataDir` + `/exports`) |
| `@TAIDAFLOW_NGINX_PORT@` | `80`(= `nginxPort`) |
| `@TAIDAFLOW_REST_PORT@` | `18080`(= `restPort`) |

(用過一次 `start-taidaflow.bat` 的話,這個檔已經自動產生在同一個位置,可以直接用。)

**(c) 啟動 / 檢查 / 停止 nginx**(nginx 在 `C:\tools\nginx\nginx-1.30.5\` 時把 `%TF%\nginx` 換成那個資料夾)

```bat
"%TF%\nginx\nginx.exe" -p "C:/TaidaFlowData/nginx/" -c conf/taidaflow.conf -t
start "TaidaFlow nginx" /min "%TF%\nginx\nginx.exe" -p "C:/TaidaFlowData/nginx/" -c conf/taidaflow.conf
curl -I http://127.0.0.1/
"%TF%\nginx\nginx.exe" -p "C:/TaidaFlowData/nginx/" -c conf/taidaflow.conf -s quit
```

- 第 1 行 `-t`:檢查設定,要看到 `syntax is ok` 與 `test is successful`。
- 第 2 行:啟動(會多一個最小化的 nginx 視窗,不要關它;關掉它 nginx 就停了)。
- 第 3 行:要看到 `HTTP/1.1 302` 與 `Location: /TaidaFlowApp.html`。app 也在執行時,`curl http://127.0.0.1/api/`
  要回 `{"status":"ok"}`(REST API 經 nginx)。
- 第 4 行:**正常停止**(`-s quit`,等傳送中的下載送完)。`-p` 必須與啟動時相同。
  設定改過或網頁更新後可用 `-s reload` 重新載入。

**(d) 停止 app**:按視窗右上角的 X 關閉,或在 cmd:

```bat
taskkill /IM TaidaFlowApp.exe
```

(不要加 `/F`:不加 `/F` 等於請視窗關閉,app 會正常收尾;`/F` 是強制結束,最後一筆資料庫寫入或
進行中的匯出可能遺失,輸出維持最後狀態。)

### 1.6 停止:順序

**雙擊 `C:\TaidaFlow\stop-taidaflow.bat`。** 它從 `config.json` 的 `dataDir` 找到執行中的 app。順序固定:

1. 先停 **nginx**(只停 `start-taidaflow` 自己啟動的那個,`nginx -s quit`);
2. 再**關閉 app 視窗**(與手動按 X 相同,app 正常收尾),最多等 60 秒。

結果碼:`0` 已停止;`1` 沒有由 start-taidaflow 啟動的 app 在執行(或 app 啟動後 `config.json` 的 `dataDir` 被改過、
或啟動時用了 `-DataDir`:這時用 `stop-taidaflow.bat -DataDir <當時的資料夾>`);`2` `config.json` 有錯;
`7` app 60 秒內沒關閉(**不會強制結束**,請到畫面上手動關);`9` nginx 停不下來(app 仍已關閉)。
真的關不掉時,管理員可以在 PowerShell 執行
`powershell -ExecutionPolicy Bypass -File C:\TaidaFlow\stop-taidaflow.ps1 -Force`
(強制結束,風險同上)。

### 1.7 開機自動啟動

app 有操作畫面,所以採「**使用者登入時**」由工作排程器執行 `start-taidaflow.ps1`
(不用 Windows 服務:服務跑在看不到畫面的 session 0,現場畫面不會出現)。

- **先乾跑**(不會註冊任何東西,只印出將要建立的排程與目前 config.json 的設定):
  ```bat
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\register-autostart.ps1 -WhatIf
  ```
- 確認內容正確後,**由要自動登入的那個使用者**執行同一行但**拿掉 `-WhatIf`**。
  排程名稱 `TaidaFlow`:登入後 30 秒執行
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\TaidaFlow\start-taidaflow.ps1"`,
  只在使用者登入時執行、一般權限、已在執行就不再開第二個。設定每次都從 `config.json` 讀,
  **改 config.json 後不用重新註冊**(只有給 `register-autostart.ps1` 的 `-DataDir`、`-UseNginx`、`-Port`、`-RestPort`
  會寫進排程,平常不要給)。
- 移除:`powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\unregister-autostart.ps1`
  (先加 `-WhatIf` 看);只移除執行 start-taidaflow.ps1 的排程,不會關掉執行中的 app。
- 不用 PowerShell 的做法:開「工作排程器」(`taskschd.msc`)→ 建立工作 → 觸發程序「登入時」
  (指定使用者、延遲 30 秒)→ 動作「啟動程式」`C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`,
  引數同上,起始位置 `C:\TaidaFlow` → 條件取消「只在使用 AC 電源時才啟動」。
- 停電復電後要自動回到畫面,電腦必須**自動登入**該使用者(Windows 自動登入設定),這是現場帳號與
  安全性的決定,本專案不設定。
- 若將來只要網頁、不要現場畫面而想做成服務:可用 WinSW 或 NSSM 包裝 `TaidaFlowApp.exe`
  (app 自己讀 config.json 的資料夾;環境變數 `TAIDAFLOW_DOWNLOAD_PORT=80`)與 nginx(`-p <資料夾>\nginx/ -c conf/taidaflow.conf`,
  停止用 `-s quit`)。**未實作也未測試**,且服務沒有現場畫面。

### 1.8 更新版本(config.json 不會被覆蓋)

新的打包資料夾**也附一份 config.json(預設值)**;更新時一定要保留現場那一份。做法二選一:

**做法 A(建議):新資料夾 + 放回 config.json**

1. 雙擊 `stop-taidaflow.bat`(nginx → app)。
2. 把舊的 `C:\TaidaFlow` 改名成 `C:\TaidaFlow.old`。
3. 把新的打包資料夾複製成 `C:\TaidaFlow`(程式、Qt、`web\` 網頁一起換新)。
4. **把 `C:\TaidaFlow.old\config.json` 複製回 `C:\TaidaFlow\config.json`(覆蓋新包附的預設值)**:
   `copy /Y C:\TaidaFlow.old\config.json C:\TaidaFlow\config.json`。新包沒有 `nginx\` 而舊的有,也一起複製回來。
5. 雙擊 `start-taidaflow.bat`;畫面上的 `settings    : dataDir=...` 一行要是現場的設定。確認沒問題後再刪 `C:\TaidaFlow.old`。
   (自動啟動的排程指向 `C:\TaidaFlow\start-taidaflow.ps1`,路徑沒變就不用重新註冊。)

**做法 B:直接覆蓋到原資料夾,排除 config.json**(停止後在 cmd 執行,`<新包>` 換成新打包資料夾的路徑)

```bat
robocopy "<新包>" "C:\TaidaFlow" /E /XF config.json
```

`/XF config.json` = 不複製 config.json,現場那一份原封不動(robocopy 結果碼 0~7 都是成功)。**不要用檔案總管
整個拖過去覆蓋**:那會把 config.json 換回預設值。

- 資料夾(`dataDir`)在程式資料夾外面,更新完全不動。從 w2-060 以前的版本(沒有 config.json、資料在
  `C:\TaidaFlow\runtime`)更新時:停止後把 `C:\TaidaFlow.old\runtime` 裡的東西**整個搬到** `C:\TaidaFlowData`,
  或把新 config.json 的 `dataDir` 改成舊資料夾;否則新版會從空的 `C:\TaidaFlowData` 開始。
- 只換網頁、nginx 繼續執行時:nginx 直接從磁碟送新檔,瀏覽器靠 ETag 自動拿新版;設定有改才需要
  `nginx.exe -p "C:/TaidaFlowData/nginx/" -c conf/taidaflow.conf -s reload`。

### 1.9 備份

停止 app 後(或在不忙的時段)複製:

- 資料夾(`dataDir`,預設 `C:\TaidaFlowData`)裡的 `data\`(所有 `sensor_YYYYMM.sqlite`:歷史資料與警報)、
  `settings.sqlite`、`TaidaFlowSettings.ini`、`device_info.ini`
- 程式資料夾的 **`config.json`**(現場設定)

`exports\` 是可重新匯出的 CSV、`logs\` 是紀錄檔,可選擇性備份。執行中直接複製 `.sqlite` 可能拿到
寫到一半的狀態,正式備份請先停止。
