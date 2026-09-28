# TaidaFlow 部署與啟動說明(正式機 / 測試機)

> 給操作人員與現場安裝人員看。不需要會 PowerShell:每個動作都寫明「開哪個檔、怎麼開」,
> 也列出**完全不用腳本**的手動做法(cmd 指令)。
> 這份文件在打包時會複製成打包資料夾裡的 `DEPLOY.md`(內容相同)。
> 來源:`taidaflow/docs/DEPLOY_AND_STARTUP.md`(w2-057)。

## 0. 先看這裡:三件最重要的事

1. **正式機(工廠現場)會控制真設備。** `TaidaFlowApp.exe` 一啟動就會連
   `192.168.1.201~205:502` 的 5 台 ADAM 模組並**寫入**輸出(泵浦、閥、變頻器、緊急停止迴路),
   開 `COM2`(MS300 變頻器),並在 `502` 開 Modbus 伺服器給外部 HMI。
2. **正式機用打包資料夾裡的檔**(`start-taidaflow.bat` 等);**開發機用 repo 的 `scripts\`**
   (`run-desktop.ps1` 等,會先做安全探測)。兩套不可混用,差異見 [§6](#6-開發機腳本與正式機腳本的差異不可混用)。
3. **網址**:網頁 `http://<電腦的 IP>/`(nginx,port 80);備援 `http://<電腦的 IP>:8124/TaidaFlowApp.html`
   (app 自己提供)。網頁要能連 `8125` 才會同步(見 [§4](#4-port-表與每個程式的用途))。

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

### 1.2 安裝(第一次)

1. 在開發機打包(開發人員做,見 [§3.5](#35-打包正式機資料夾開發人員)):得到
   `dist\TaidaFlow-<日期>-<版本>\` 整個資料夾。
2. 把**整個資料夾**複製到正式機並改名,建議固定放在 **`C:\TaidaFlow\`**(也就是
   `C:\TaidaFlow\TaidaFlowApp.exe`、`C:\TaidaFlow\start-taidaflow.bat`;以下都用這個路徑。版本記在
   `VERSION.txt`。不要放在 `C:\Program Files`,那裡一般使用者不能寫入)。
3. 選資料夾(DataDir,app 的工作目錄):
   - 預設 `C:\TaidaFlow\runtime\`(什麼都不改就是這個)。
   - **建議**放在程式資料夾外面,例如 `C:\TaidaFlowData\` 或 `D:\TaidaFlowData\`,以後更新程式
     就不會碰到資料。做法:把 `C:\TaidaFlow\taidaflow-site.example.bat` **複製**成
     `C:\TaidaFlow\taidaflow-site.bat`,用記事本打開,改 `set "DATADIR=C:\TaidaFlowData"`(路徑結尾不要加 `\`)。
     `start-taidaflow.bat` / `stop-taidaflow.bat` 都會讀這個檔。
4. 防火牆(由**管理員**設定,見 [§1.9](#19-防火牆由管理員設定))。
5. 第一次啟動(§1.3)後,資料夾裡會出現:

   | 檔案 / 資料夾 | 內容 |
   |---|---|
   | `TaidaFlowSettings.ini` | 設定(`[Alarm] aiHighAlarmPercent` 等) |
   | `settings.sqlite` | 感測器名稱、讀取頻率等設定 |
   | `data\sensor_YYYYMM.sqlite` | 歷史資料與警報,每月一個檔 |
   | `exports\` | 網頁匯出的 CSV(超過 20 個或 2 GB 時自動刪最舊的) |
   | `logs\` | `launcher.log`(啟動/停止紀錄)與每次啟動一個 `taidaflow-<日期-時間>.log` |
   | `nginx\` | nginx 的設定(`conf\taidaflow.conf`,自動產生)、log、暫存 |
   | `taidaflow-app.json` | 執行中的 app 身分(停止時用;停止後刪除) |

   (`settings_schema.sql` / `data_schema.sql` 不需要:找不到時 app 用內建結構,log 會有一行
   `Schema file not found`,屬正常。)

### 1.3 啟動:要開哪個檔

**雙擊 `C:\TaidaFlow\start-taidaflow.bat`。**

- 它會:檢查是否已在執行 / port 被占用 → 啟動 nginx(port 80)→ 啟動 `TaidaFlowApp.exe`
  → 確認 8124、8125 在聽 → 顯示網址與結果碼,按任意鍵關閉這個黑色視窗(app 會繼續執行)。
- 結果碼:`0` 成功;`4` 已經在執行或 port 被占用(**什麼都沒啟動**,也不會關掉別的程式);
  `6` app 沒起來(看 `logs\` 裡最新的 log);`8` app 已啟動但 nginx 沒起來(網頁改用 `:8124`);
  `2` 打包資料夾不完整或資料夾不能寫。
- 不要直接雙擊 `TaidaFlowApp.exe`:雖然正式機資料夾裡的 exe 可以直接跑,但工作目錄會變成
  「目前資料夾」、下載連結也不會指到 nginx(port 80)。要手動啟動請照 §1.4。

### 1.4 不用任何腳本的手動做法(cmd)

打開「命令提示字元」(開始 → 輸入 `cmd`),**一行一行**貼上。先設定兩個路徑:

```bat
set "TF=C:\TaidaFlow"
set "DATA=C:\TaidaFlowData"
```

**(a) 啟動 app**

```bat
mkdir "%DATA%" 2>nul
cd /d "%DATA%"
set TAIDAFLOW_DEVICE_PROFILE=
set TAIDAFLOW_E2E_PV_FILE=
set QT_PLUGIN_PATH=
set QML_IMPORT_PATH=
set QML2_IMPORT_PATH=
set TAIDAFLOW_DOWNLOAD_PORT=80
start "" "%TF%\TaidaFlowApp.exe"
```

- `cd /d "%DATA%"`:**工作目錄**就是資料夾,app 的設定、資料庫、匯出都寫在這裡。
- `set TAIDAFLOW_DEVICE_PROFILE=`(等號後面空白)= 清除測試用的模擬器設定,正式機**一定**要清掉。
- `TAIDAFLOW_DOWNLOAD_PORT=80`:網頁上的 CSV 下載連結走 nginx(port 80);不設就走 app 的 8124。
- 確認:`netstat -ano | findstr LISTENING | findstr ":8124 :8125 :502"` 要看到這三個 port。

**(b) nginx 第一次:準備設定檔**(之後不用再做)

```bat
mkdir "%DATA%\nginx\conf" "%DATA%\nginx\logs" "%DATA%\nginx\temp" "%DATA%\exports"
copy "%TF%\deploy\nginx\taidaflow.conf" "%DATA%\nginx\conf\taidaflow.conf"
notepad "%DATA%\nginx\conf\taidaflow.conf"
```

在記事本用「取代」(Ctrl+H)換掉三個記號,路徑一律用**正斜線 `/`**,存檔:

| 記號 | 換成(以本節路徑為例) |
|---|---|
| `@TAIDAFLOW_WEB_ROOT@` | `C:/TaidaFlow/web` |
| `@TAIDAFLOW_EXPORT_DIR@` | `C:/TaidaFlowData/exports` |
| `@TAIDAFLOW_NGINX_PORT@` | `80` |

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
- 第 3 行:要看到 `HTTP/1.1 302` 與 `Location: /TaidaFlowApp.html`。
- 第 4 行:**正常停止**(`-s quit`,等傳送中的下載送完)。`-p` 必須與啟動時相同。
  設定改過或網頁更新後可用 `-s reload` 重新載入。

**(d) 停止 app**:按視窗右上角的 X 關閉,或在 cmd:

```bat
taskkill /IM TaidaFlowApp.exe
```

(不要加 `/F`:不加 `/F` 等於請視窗關閉,app 會正常收尾;`/F` 是強制結束,最後一筆資料庫寫入或
進行中的匯出可能遺失,輸出維持最後狀態。)

### 1.5 停止:順序

**雙擊 `C:\TaidaFlow\stop-taidaflow.bat`。** 順序固定:

1. 先停 **nginx**(只停 `start-taidaflow` 自己啟動的那個,`nginx -s quit`);
2. 再**關閉 app 視窗**(與手動按 X 相同,app 正常收尾),最多等 60 秒。

結果碼:`0` 已停止;`1` 沒有由 start-taidaflow 啟動的 app 在執行(或資料夾設定不同,見 `taidaflow-site.bat`);
`7` app 60 秒內沒關閉(**不會強制結束**,請到畫面上手動關);`9` nginx 停不下來(app 仍已關閉)。
真的關不掉時,管理員可以在 PowerShell 執行
`powershell -ExecutionPolicy Bypass -File C:\TaidaFlow\stop-taidaflow.ps1 -DataDir C:\TaidaFlowData -Force`
(強制結束,風險同上)。

### 1.6 開機自動啟動

app 有操作畫面,所以採「**使用者登入時**」由工作排程器執行 `start-taidaflow.ps1`
(不用 Windows 服務:服務跑在看不到畫面的 session 0,現場畫面不會出現)。

- **先乾跑**(不會註冊任何東西,只印出將要建立的排程):
  ```bat
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\register-autostart.ps1 -WhatIf -DataDir C:\TaidaFlowData -UseNginx -Port 80
  ```
- 確認內容正確後,**由要自動登入的那個使用者**執行同一行但**拿掉 `-WhatIf`**。
  排程名稱 `TaidaFlow`:登入後 30 秒執行
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\TaidaFlow\start-taidaflow.ps1" -DataDir "C:\TaidaFlowData" -UseNginx -Port 80`,
  只在使用者登入時執行、一般權限、已在執行就不再開第二個。
- 移除:`powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\unregister-autostart.ps1`
  (先加 `-WhatIf` 看);只移除執行 start-taidaflow.ps1 的排程,不會關掉執行中的 app。
- 不用 PowerShell 的做法:開「工作排程器」(`taskschd.msc`)→ 建立工作 → 觸發程序「登入時」
  (指定使用者、延遲 30 秒)→ 動作「啟動程式」`C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`,
  引數同上,起始位置 `C:\TaidaFlow` → 條件取消「只在使用 AC 電源時才啟動」。
- 停電復電後要自動回到畫面,電腦必須**自動登入**該使用者(Windows 自動登入設定),這是現場帳號與
  安全性的決定,本專案不設定。
- 若將來只要網頁、不要現場畫面而想做成服務:可用 WinSW 或 NSSM 包裝 `TaidaFlowApp.exe`
  (工作目錄 = 資料夾、環境變數 `TAIDAFLOW_DOWNLOAD_PORT=80`)與 nginx(`-p <資料夾>\nginx/ -c conf/taidaflow.conf`,
  停止用 `-s quit`)。**未實作也未測試**,且服務沒有現場畫面。

### 1.7 更新版本

1. 雙擊 `stop-taidaflow.bat`(nginx → app)。
2. 把舊的 `C:\TaidaFlow` 改名成 `C:\TaidaFlow.old`。
3. 把新的打包資料夾複製成 `C:\TaidaFlow`(程式、Qt、`web\` 網頁一起換新)。
4. 從 `C:\TaidaFlow.old` 把 `taidaflow-site.bat`(有的話)複製回 `C:\TaidaFlow`;新包沒有 `nginx\`
   而舊的有,也一起複製回來。
5. **資料夾(DataDir)不動**:放在 `C:\TaidaFlowData` 就完全不用管。若用的是預設
   `C:\TaidaFlow\runtime`,要把 `C:\TaidaFlow.old\runtime` **整個搬回** `C:\TaidaFlow\runtime`。
6. 雙擊 `start-taidaflow.bat`。確認沒問題後再刪 `C:\TaidaFlow.old`。
   (自動啟動的排程指向 `C:\TaidaFlow\start-taidaflow.ps1`,路徑沒變就不用重新註冊。)
7. 只換網頁、nginx 繼續執行時:nginx 直接從磁碟送新檔,瀏覽器靠 ETag 自動拿新版;設定有改才需要
   `nginx.exe -p "C:/TaidaFlowData/nginx/" -c conf/taidaflow.conf -s reload`。

### 1.8 備份

停止 app 後(或在不忙的時段)複製資料夾裡的:

- `data\`(所有 `sensor_YYYYMM.sqlite`:歷史資料與警報)
- `settings.sqlite`
- `TaidaFlowSettings.ini`(以及 `device_info.ini`,若存在)
- 程式資料夾的 `taidaflow-site.bat`(若有)

`exports\` 是可重新匯出的 CSV、`logs\` 是紀錄檔,可選擇性備份。執行中直接複製 `.sqlite` 可能拿到
寫到一半的狀態,正式備份請先停止。

### 1.9 防火牆(由管理員設定)

本專案的腳本**不會**改防火牆。需要開的輸入 port:

| port | 為什麼 | 對誰開 |
|---|---|---|
| **80** | 網頁(nginx)與 CSV 下載 | 要看網頁的區網電腦 |
| **8125** | 網頁與桌面同步(WebSocket),**不開網頁會一直顯示「離線」** | 同上 |
| **502** | 外部 HMI 讀寫 Modbus | 只給 HMI 的 IP |
| 8124 | 備援網址 `:8124`(app 自己的網頁與下載) | 可選;只用 port 80 時可以不開 |
| 18125 | 內部用(只綁 127.0.0.1) | **不要開** |

管理員在「以系統管理員身分執行」的 cmd 中執行(範例,IP 請依現場修改):

```bat
netsh advfirewall firewall add rule name="TaidaFlow web (nginx 80)" dir=in action=allow protocol=TCP localport=80 profile=domain,private
netsh advfirewall firewall add rule name="TaidaFlow web sync (8125)" dir=in action=allow protocol=TCP localport=8125 profile=domain,private
netsh advfirewall firewall add rule name="TaidaFlow Modbus HMI (502)" dir=in action=allow protocol=TCP localport=502 remoteip=192.168.1.50 profile=domain,private
netsh advfirewall firewall add rule name="TaidaFlow web fallback (8124)" dir=in action=allow protocol=TCP localport=8124 profile=domain,private
```

第一次有程式在 `0.0.0.0` 開 port 時,Windows 可能跳出「允許存取」的詢問視窗,由管理員決定。

### 1.10 網址

| 網址 | 由誰提供 |
|---|---|
| `http://<IP>/` | nginx(port 80),自動轉到 `/TaidaFlowApp.html` |
| `http://<IP>/TaidaFlowApp.html` | 同上 |
| `http://<IP>:8124/TaidaFlowApp.html` | app 自己(備援;nginx 沒跑時也能用) |
| `http://<IP>/exports/<檔名>.csv` | 匯出 CSV 下載(nginx,支援續傳);`:8124/exports/...` 為備援(不支援續傳) |

`<IP>` 用 `ipconfig` 查本機的 IPv4。`start-taidaflow.bat` 結束時也會列出所有網址。

---

## 2. 網頁版(Qt WebAssembly)

網頁是同一份 Qt 程式編成 WebAssembly,在瀏覽器裡跑,**本身不連設備**,資料全部經由 port 8125
與桌面 app 同步。

### 2.1 建置(開發機)

```bat
scripts\build-wasm.bat
```

等同於(手動):

```bat
call C:\tools\emsdk\emsdk_env.bat
cd /d D:\repo\codex\qmlTester\taidaflow
C:\Qt\Tools\CMake_64\bin\cmake.exe --preset wasm-release
C:\Qt\Tools\CMake_64\bin\cmake.exe --build --preset wasm-release
```

- Qt kit:`C:\Qt\6.8.3\wasm_singlethread`;編譯器:emsdk **3.1.56**(`C:\tools\emsdk`);
  preset `wasm-release`(`CMakePresets.json`),輸出 `build\wasm-release\`。
- 產出(網頁要用的只有這 5 個):`TaidaFlowApp.html`、`TaidaFlowApp.js`、
  `TaidaFlowApp.wasm`(約 33.8 MB)、`qtloader.js`、`qtlogo.svg`。資料夾裡其他檔(CMake/Ninja)不要部署。
- **載入畫面**(w2-058):`TaidaFlowApp.html` 不是 Qt 預設頁,而是 `App\wasm\TaidaFlowApp.shell.html`
  (旋轉立方體 + 「TAIDAFLOW」+ 繁中狀態);建置時自動套用(configure log 有 `[wasm-shell]` 兩行)。
  要改畫面或文字:改這個樣板 → 重新 `scripts\build-wasm.bat` → 重新 `scripts\deploy-web.ps1`(§2.2)。
  `qtlogo.svg` 新頁面已不用,照常複製無影響。
- 改過中文字串(QML/C++)後,先檢查網頁內嵌字型是否涵蓋所有字:
  `python -B scripts\make_font_subset.py --check`(exit 0 = 涵蓋;exit 1 = 要執行
  `python scripts\make_font_subset.py` 重新產生字型,再重新建置 wasm)。

### 2.2 部署到 exe 旁邊的 `web\`

```bat
powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1
```

- 預設把 `build\wasm-release` 的上述網頁檔複製到 `build\desktop\web\`(`-ExeDir` 可改),先**清空**舊的
  `web\`,並為 `.html/.js/.wasm/.svg` 產生預先壓縮的 `.gz`(`.wasm` 約 12.4 MB);打包
  (`package-release.ps1`)用同一支腳本產生打包資料夾的 `web\`。
- 手動等價做法(不壓縮也能用,只是傳輸量較大):
  ```bat
  rmdir /s /q build\desktop\web
  mkdir build\desktop\web
  for %f in (TaidaFlowApp.html TaidaFlowApp.js TaidaFlowApp.wasm qtloader.js qtlogo.svg) do copy build\wasm-release\%f build\desktop\web\
  ```
  **一定要先刪掉整個 `web\`**:舊的 `.gz` 若留著,nginx 會送舊版(nginx 不比對檔案時間)。
  (這是直接在 cmd 視窗輸入的寫法;寫進 `.bat` 檔時 `%f` 要改成 `%%f`。)

### 2.3 誰在送網頁、怎麼開

- nginx:`http://<IP>/`(port 80,網頁資料夾 = `<exe 資料夾>\web`)。
- app:`http://<IP>:8124/TaidaFlowApp.html`(找網頁資料夾的順序:環境變數 `TAIDAFLOW_WEB_DIR` →
  `<exe 資料夾>\web` → 開發機的 `build\wasm-release`)。
- 兩者都帶 `Cross-Origin-Opener-Policy` / `Cross-Origin-Embedder-Policy` 標頭、`.wasm` 以
  `Content-Encoding: gzip` 傳送、每次以 ETag 重新驗證。

### 2.4 網頁怎麼連回桌面

網頁載入後連 `ws://<載入網頁的同一個主機>:8125/mirror`(app 的區網轉發,轉到本機 18125 的
mirror)。連上並收到完整狀態後畫面才可操作;**畫面上方紅色「離線」橫幅 = 沒有同步**,可能原因:
app 沒在執行、8125 被防火牆擋、8125 被別的程式占用(app log 會有 `LAN relay could not listen`)。

### 2.5 網頁更新

重新建置(§2.1)→ 重新部署(§2.2,或重新打包後照 §1.7 更新)→ nginx 執行中時可
`-s reload`(只有設定改變才需要;新檔 nginx 直接從磁碟送)。瀏覽器因 `Cache-Control: no-cache` +
ETag 每次都重新驗證,重新整理就拿到新版,不會新舊混用。app 的 8124 若是在 `web\` 還不存在時啟動的,
部署後要重新啟動 app 才會開始送網頁。

---

## 3. 測試機(開發機接模擬器)

開發機**沒有**真設備,用 `Adam60xxSimulator` 模擬 5 台 ADAM(`127.0.0.201~205:502`)。

### 3.1 為什麼不能直接雙擊 `build\desktop\TaidaFlowApp.exe`

- 開發 build 的資料夾**沒有 Qt 的 DLL**(它們在 `C:\Qt\6.8.3\msvc2022_64\bin`,不在一般的 PATH 裡),
  雙擊會出現「找不到 Qt6Core.dll」之類的系統錯誤。可用 `dumpbin /dependents TaidaFlowApp.exe` 看到它需要
  `Qt6Core.dll`、`Qt6Qml.dll` 等。更糟的情況:PATH 上有別的軟體附的 `Qt6Core.dll`(例如這台開發機的
  `C:\Program Files\Basler\FramegrabberSDK\bin`),雙擊會載入**別的版本**的 Qt,可能直接當掉或出現「找不到程序進入點」。
  (正式機打包資料夾不受影響:exe 旁邊的 DLL 優先,`start-taidaflow` 也會把 PATH 中含 `Qt6Core.dll` 的資料夾移除。)
- 就算 PATH 有 Qt:工作目錄會是 `build\desktop`(設定與資料庫寫錯地方),而且**沒有設
  `TAIDAFLOW_DEVICE_PROFILE=simulator` 時 app 連的是真設備位址 `192.168.1.201~205:502`**、會開 `COM2`。
  開發機若正好接在工廠網段,就會**直接控制真設備**。所以測試機一律照下面的步驟(或用腳本,§3.4)。

### 3.2 手動步驟(cmd,不用 .ps1)

開一個 cmd 視窗,一行一行執行(路徑依你的 repo 位置):

```bat
set "REPO=D:\repo\codex\qmlTester"
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
```

**0. 先確認沒有人在用這些 port**(有輸出 = 有程式在用,先查清楚,不要關別人的程式):

```bat
netstat -ano | findstr LISTENING | findstr /C:":80 " /C:":502 " /C:":8124 " /C:":8125 " /C:":18125 "
```

**1. 模擬器**(一定要**先**開模擬器,再開 app)

```bat
mkdir "%REPO%\taidaflow\build\sim-cwd" 2>nul
cd /d "%REPO%\taidaflow\build\sim-cwd"
start "" "%REPO%\Adam60xxSimulator\build\Adam60xxSimulator.exe" --autostart
netstat -ano | findstr LISTENING | findstr ":502 "
```

要看到 `127.0.0.201:502` ~ `127.0.0.205:502` 五行 `LISTENING`。

**1b. 安全探測(建議,這一步是 .ps1)**:確認 `192.168.1.201~205` 連不到、沒有 COM2:

```bat
powershell -ExecutionPolicy Bypass -File "%REPO%\taidaflow\scripts\safety_probe.ps1" -DeviceProfile simulator -Reason "manual"
```

要看到 `verdict: SAFE`(exit 0)才繼續。

**2. TaidaFlowApp**

```bat
set TAIDAFLOW_DEVICE_PROFILE=simulator
set TAIDAFLOW_DOWNLOAD_PORT=80
mkdir "%REPO%\taidaflow\build\runtime-cwd" 2>nul
cd /d "%REPO%\taidaflow\build\runtime-cwd"
start "" "%REPO%\taidaflow\build\desktop\TaidaFlowApp.exe"
netstat -ano | findstr LISTENING | findstr ":8124 :8125 :18125"
```

- `TAIDAFLOW_DEVICE_PROFILE=simulator`:ADAM 連線改到 `127.0.0.201~205`(模擬器)。**這一行不能漏**(§3.1)。
- 工作目錄 `build\runtime-cwd`(已被 git 忽略)。
- 要留 log:在 `start` 前加 `set QT_FORCE_STDERR_LOGGING=1`,並把最後一行換成
  `start "" /b cmd /c ""%REPO%\taidaflow\build\desktop\TaidaFlowApp.exe" 2> "%REPO%\taidaflow\build\runtime-logs\manual.log""`。

**3. nginx(port 80)**:第一次先準備設定檔(同 §1.4 (b),路徑換成測試機的):

```bat
mkdir "%REPO%\taidaflow\build\nginx-manual\conf" "%REPO%\taidaflow\build\nginx-manual\logs" "%REPO%\taidaflow\build\nginx-manual\temp"
copy "%REPO%\taidaflow\deploy\nginx\taidaflow.conf" "%REPO%\taidaflow\build\nginx-manual\conf\taidaflow.conf"
notepad "%REPO%\taidaflow\build\nginx-manual\conf\taidaflow.conf"
```

取代:`@TAIDAFLOW_WEB_ROOT@` → `D:/repo/codex/qmlTester/taidaflow/build/desktop/web`、
`@TAIDAFLOW_EXPORT_DIR@` → `D:/repo/codex/qmlTester/taidaflow/build/runtime-cwd/exports`、
`@TAIDAFLOW_NGINX_PORT@` → `80`。然後:

```bat
set "NGX=C:\tools\nginx\nginx-1.30.5\nginx.exe"
"%NGX%" -p "D:/repo/codex/qmlTester/taidaflow/build/nginx-manual/" -c conf/taidaflow.conf -t
start "TaidaFlow nginx" /min "%NGX%" -p "D:/repo/codex/qmlTester/taidaflow/build/nginx-manual/" -c conf/taidaflow.conf
```

(`build\nginx-manual` 與腳本用的 `build\nginx` 分開,手動與腳本不會互相覆蓋。)

**4. 開網頁**:瀏覽器開 `http://127.0.0.1/`(或 `http://<本機 IP>/`);備援 `http://127.0.0.1:8124/TaidaFlowApp.html`。
不開瀏覽器的檢查:

```bat
curl -I http://127.0.0.1/
curl -I http://127.0.0.1/TaidaFlowApp.html
curl -I http://127.0.0.1:8124/TaidaFlowApp.html
```

**5. 關閉**(順序:nginx → app → 模擬器)

```bat
"%NGX%" -p "D:/repo/codex/qmlTester/taidaflow/build/nginx-manual/" -c conf/taidaflow.conf -s quit
taskkill /IM TaidaFlowApp.exe
taskkill /IM Adam60xxSimulator.exe
netstat -ano | findstr LISTENING | findstr /C:":80 " /C:":502 " /C:":8124 " /C:":8125 " /C:":18125 "
```

程式收尾需要幾秒,等 5 秒左右再執行最後一行;最後一行沒有輸出 = 全部關乾淨。`taskkill` 不加 `/F`
(等於關視窗,程式正常收尾)。

### 3.3 對應的 .ps1 腳本(替代做法)

| 手動步驟 | 腳本(repo 的 `scripts\`) |
|---|---|
| 1. 模擬器 | `powershell -ExecutionPolicy Bypass -File scripts\run-simulator.ps1` |
| 1b + 2. 探測 + app | `$env:TAIDAFLOW_DOWNLOAD_PORT = '80'` 後 `powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -DeviceProfile simulator -Label "sim"`(先安全探測,SAFE 才啟動) |
| 3. nginx | `powershell -ExecutionPolicy Bypass -File scripts\nginx-start.ps1`(port 80;`-Port 8123` 改用舊 port) |
| 5. 關 nginx | `powershell -ExecutionPolicy Bypass -File scripts\nginx-stop.ps1`(只停自己起的) |
| 網頁部署 | `powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1` |

### 3.4 正式機打包資料夾在開發機上的驗證

`powershell -ExecutionPolicy Bypass -File scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<...>`
(先跑安全探測,SAFE 才用「沒有 Qt 的 PATH」啟動打包資料夾,以 curl 檢查 80/8124/8125、下載與 Range,
最後停止並確認沒有殘留;不操作畫面。)

### 3.5 打包正式機資料夾(開發人員)

```bat
scripts\build-desktop.bat
scripts\build-wasm.bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-release.ps1 -IncludeNginx
```

產出 `dist\TaidaFlow-<yyyyMMdd>-<git 短雜湊>\`(`dist\` 不進 git)。內容:`TaidaFlowApp.exe`、
windeployqt(Qt 6.8.3)帶入的 Qt DLL / plugins / `qml\`、MSVC 執行環境 DLL、`web\`(含 `.gz`)、
`start/stop-taidaflow.bat/.ps1`、`register/unregister-autostart.ps1`、`taidaflow-site.example.bat`、
`logging\quiet.ini`、`scripts\nginx-web.ps1`、`deploy\nginx\taidaflow.conf`、`DEPLOY.md`(本文件)、
`VERSION.txt`、`MANIFEST.txt`(每個檔的大小與 SHA-256)、`nginx\`(`-IncludeNginx` 時)。
不含資料庫、ini、log、測試或開發檔。build 不是最新時拒絕打包(exit 3)。
DLL 相依檢查:`powershell -ExecutionPolicy Bypass -File scripts\check-package-deps.ps1 -Package dist\TaidaFlow-<...>`。

---

## 4. port 表與每個程式的用途

| port / 位址 | 程式 | 用途 | 區網要開嗎 |
|---|---|---|---|
| `0.0.0.0:80` | nginx(另一個程式,`start-taidaflow` 或 `nginx-start.ps1` 啟動) | 網頁 `http://<IP>/`、CSV 下載 `/exports/`(續傳) | 要(看網頁的人) |
| `0.0.0.0:502` | TaidaFlowApp(Modbus TCP 伺服器) | 外部 HMI 讀寫 | 只給 HMI |
| `0.0.0.0:8124` | TaidaFlowApp(內建 HTTP) | 網頁與 CSV 下載的備援 `:8124` | 可選 |
| `0.0.0.0:8125` | TaidaFlowApp(區網轉發) | 網頁 ↔ 桌面同步(WebSocket `/mirror`) | 要 |
| `127.0.0.1:18125` | TaidaFlowApp(mirror 伺服器) | 內部,8125 轉到這裡 | 不要 |
| 連出 `192.168.1.201~205:502` | TaidaFlowApp(Modbus 用戶端) | 5 台 ADAM(正式機);測試機設模擬器時改 `127.0.0.201~205` | — |
| `COM2` | TaidaFlowApp | MS300 變頻器(Modbus RTU) | — |
| `127.0.0.201~205:502` | Adam60xxSimulator(只在測試機) | 模擬 5 台 ADAM | — |
| `8123` | (舊)nginx 的前一個 port | 需要時 `-Port 8123` 仍可用 | — |

---

## 5. 常見問題

**網頁上方顯示紅色「離線」**:網頁連不到桌面(port 8125)。檢查:app 有沒有在執行;
`netstat -ano | findstr ":8125"` 有沒有 `LISTENING`;防火牆有沒有開 8125;app log 有沒有
`LAN relay could not listen on 0.0.0.0:8125`(8125 被別的程式占用)。橫幅期間操作鈕會停用,連回後自動恢復。

**啟動時說 port 被占用(exit 4)**:畫面/`logs\launcher.log` 會列出占用者(port、pid、程式路徑)。
不會關掉別人的程式。常見情況:
- `TaidaFlowApp is already running`:已經開著了(先 stop-taidaflow 或手動關)。
- port 80 的占用者是 **pid 4「System」**:那是 Windows 的 HTTP.sys(IIS 的 World Wide Web 發佈服務、
  SQL Server Reporting Services、網頁部署代理程式、某些 URL 保留等)。用
  `netsh http show servicestate` 看是誰;由管理員停用那個服務,或改用別的 port
  (`taidaflow-site.bat` 設 `NGINX_PORT=8123`,網址變成 `http://<IP>:8123/`,防火牆也要改開 8123)。
- port 502 被占用:可能是另一套 Modbus 軟體;兩者不能同時用 502。

**畫面文字變成方框**:
- 桌面畫面:Windows 缺中文字型。確認 `C:\Windows\Fonts` 有「微軟正黑體」(`msjh.ttc`);沒有時由管理員在
  「設定 → 應用程式 → 選用功能」加入「中文(繁體)補充字型」或安裝繁體中文語言套件。
- 網頁:網頁內嵌的是 Noto Sans TC **子集**,新加的中文字若不在子集裡會變方框 → 開發人員執行
  `python -B scripts\make_font_subset.py --check`(exit 1 表示要重新產生字型並重新建置網頁,§2.1)。

**nginx 起不來(`start-taidaflow` 結果碼 8,或 `nginx-web.ps1` 的 exit code)**:

| exit | 意思 | 怎麼辦 |
|---|---|---|
| 0 | 成功 | — |
| 1 | 沒有由腳本啟動的 nginx 在執行(stop / reload / status) | 正常情況之一 |
| 2 | 找不到 nginx.exe、網頁資料夾、`TaidaFlowApp.html` 或設定樣板,或路徑含 `$ " ' { } ; #` | 確認 `nginx\nginx.exe` 或 `C:\tools\nginx`、`web\` 存在;換不含這些字元的路徑 |
| 3 | `nginx -t` 設定檢查失敗 | 看輸出的錯誤行(手動改過設定檔時最常見) |
| 4 | port(預設 80)已被占用,或已在執行 | 見上一題;占用者會列出,不會被關掉 |
| 5 | 網頁或匯出資料夾裡有捷徑連結 / junction | 移除連結(Windows 版 nginx 會跟著連結送出資料夾外的檔) |
| 6 | 啟動了但時限內沒在聽 port | 看 `<資料夾>\nginx\logs\error.log` |
| 7 | 停止失敗 | 看輸出;可在工作管理員確認 nginx 程序 |

nginx 沒起來時 app 仍會啟動,網頁改用 `http://<IP>:8124/TaidaFlowApp.html`,下載連結也改走 8124。

**`start-taidaflow` 結果碼 6**:app 沒起來或 8124/8125 沒在聽。看 `logs\taidaflow-<時間>.log`(預設只記警告與錯誤;
要完整紀錄把 `-AppLog full` 加到啟動參數,注意 log 大約每小時 15 MB)。

**log 裡很多 `Device is not connected` / `[MS300] ... serial device does not exist`**:連不到 ADAM 或沒有 COM2
(例如在辦公室測試)。正式機接好設備後就不會一直出現。

**雙擊 .bat 出現「無法載入,因為這個系統上已停用指令碼執行」**:.bat 已用 `-ExecutionPolicy Bypass`;
若仍被擋,是公司群組原則(GPO)禁止,請 IT 放行 `C:\TaidaFlow` 的腳本。

---

## 6. 開發機腳本與正式機腳本的差異(不可混用)

| | 開發機(repo `scripts\`) | 正式機(打包資料夾) |
|---|---|---|
| 啟動 | `run-desktop.ps1`:**先跑安全探測**,只要 192.168.1.201~205 連得到、有 COM2、或 502 被占用就**拒絕**啟動(刻意的開發機保護) | `start-taidaflow.bat/.ps1`:**不做安全探測**(現場本來就要連真設備),只檢查重複執行與 port |
| Qt | 從 `C:\Qt\6.8.3\msvc2022_64\bin`(腳本加到 PATH) | 打包資料夾自帶;PATH 中含 Qt 的資料夾會被移除 |
| 工作目錄 | 固定 `build\runtime-cwd` | `-DataDir`(預設 `<安裝資料夾>\runtime`) |
| 設備 | 可用 `-DeviceProfile simulator` 改連模擬器 | **一律清除** `TAIDAFLOW_DEVICE_PROFILE` 與 `TAIDAFLOW_E2E_PV_FILE`(不能進測試模式) |
| nginx | `nginx-start.ps1`,runtime `build\nginx` | `start-taidaflow -UseNginx`,runtime `<資料夾>\nginx` |
| log | `build\runtime-logs\`(完整) | `<資料夾>\logs\`(預設只記警告與錯誤,保留 30 天) |

在正式機跑開發機腳本:安全探測一定判定不安全,app 不會啟動。在開發機跑正式機腳本:**沒有保護**,
若開發機連得到 192.168.1.201~205 就會控制真設備——開發機請只用 `scripts\` 的腳本(或 §3.2 的手動步驟)。

---

## 7. 仍需 Mango 決定的事項

1. **資料夾位置**:維持預設 `C:\TaidaFlow\runtime`,或統一用 `C:\TaidaFlowData` / `D:\TaidaFlowData`(建議)。
2. **nginx 隨包附帶**(`-IncludeNginx`,已附授權檔)或現場另外安裝到 `C:\tools\nginx`;port 維持 80 或改其他。
3. **VC++ 執行環境**:目前用 app-local DLL(免安裝);若要由 Windows Update 維護安全更新,改為現場安裝 `vc_redist.x64.exe`。
4. **開機自動啟動**:是否註冊登入排程;現場電腦是否設定 Windows 自動登入(帳號與安全性)。
5. **app log 模式**:預設 `quiet`(只記警告/錯誤)+ 保留 30 天,或 `full`(約 15 MB/小時)。
6. **防火牆範圍**:80/8125 開給哪些網段;8124 是否也開;502 只開給 HMI 的 IP。
7. **存取控管**:80/8124/8125 目前不做登入或來源限制(內網決定),任何連得到的人都能操作(含急停);正式上線前再確認。
8. **現場電腦設定**:關閉睡眠、Windows Update 自動重新開機時段、螢幕保護等(由 IT 決定)。
9. **設備位址**:ADAM 位址固定 `192.168.1.201~205`(程式內),現場網段必須一致;是否要做成可設定(需改 Core)。
10. **備份**:備份頻率與存放位置(§1.8)。
