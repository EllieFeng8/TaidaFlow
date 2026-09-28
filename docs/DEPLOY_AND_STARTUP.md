# TaidaFlow 部署與啟動說明(正式機 / 測試機)

> 給操作人員與現場安裝人員看。不需要會 PowerShell:每個動作都寫明「開哪個檔、怎麼開」,
> 也有**完全不用腳本**的手動部署(§5)。
> 這份文件在打包時會複製成打包資料夾裡的 `DEPLOY.md`(內容相同)。
> 來源:`taidaflow/docs/DEPLOY_AND_STARTUP.md`(w2-057 起,w2-062 改為 config.json 與隨包 nginx,w2-065 改為程式自己寫 log,§13)。
> 文中的 `<安裝資料夾>` 是放打包資料夾的地方(建議 `C:\TaidaFlow`),`<資料資料夾>` 是 config.json 的 `dataDir`
> (預設 `C:\TaidaFlowData`),`<repo>` 是開發機上的原始碼資料夾。

## 0. 先看這裡:五件最重要的事

1. **正式機(工廠現場)會控制真設備。** `TaidaFlowApp.exe` 一啟動就會連 config.json 裡 5 台 ADAM 模組
   (預設 `192.168.1.201~205:502`)並**寫入**輸出(泵浦、閥、變頻器、緊急停止迴路),開 MS300 變頻器的序列埠
   (預設 `COM2`),並開 Modbus 伺服器給外部 HMI(預設 port `502`)。
2. **所有設定都在 `<安裝資料夾>\config.json`**:資料資料夾、設備位址、各服務 port、nginx。第一次啟動時自動用預設值建立;
   用記事本修改;更新版本時**不會被覆蓋**(打包資料夾裡沒有這個檔)。欄位表見 §6。
3. **nginx 隨包附在 `<安裝資料夾>\nginx\`**,用 nginx 的標準做法啟動:`cd <安裝資料夾>\nginx` 然後 `start nginx`。
   `start-taidaflow.bat` 在 nginx 沒執行時也會這樣把它帶起來;`stop-taidaflow.bat` **不會**停 nginx。
4. **網址**:網頁 `http://<電腦的 IP>/`(nginx,port 80;網頁同步、下載、REST API 全部經 port 80)。
   正式機防火牆**只開 80 與 502**(§7)。
5. **正式機用打包資料夾裡的檔**(`start-taidaflow.bat` 等);**開發機用 repo 的 `scripts\`**
   (`run-desktop.ps1` 等,會先做安全探測)。兩套不可混用(§11)。部署與執行都**不需要 Python**,也不需要安裝 Qt 或 VC++ 執行環境。

出問題時先看 **log 資料夾**(預設 `C:\TaidaFlowData\logs`,config.json 的 `log.dir`):程式自己寫的
`taidaflow-<日期>.log`(警告與錯誤)與 `taidaflow-<日期>-full.log`(全部訊息),以及啟動 / 停止腳本的 `launcher-<日期>.log`(§13)。

---

## 1. 打包資料夾裡有什麼

開發人員用 `scripts\package-release.ps1` 產生 `dist\TaidaFlow-<日期>-<版本>\`,整個資料夾就是安裝內容:

| 檔案 / 資料夾 | 內容 |
|---|---|
| `TaidaFlowApp.exe` | 程式(桌面畫面 + 所有後端) |
| `Qt6*.dll`、`platforms\`、`styles\`、`imageformats\`、`sqldrivers\`、`tls\`、`qml\` … | Qt 6.8.3 的執行檔案(windeployqt) |
| `vcruntime140*.dll`、`msvcp140*.dll` … | MSVC 執行環境(隨程式放,app-local,**不用安裝 vc_redist**) |
| `web\` | 網頁版(WebAssembly)+ 預先壓縮的 `.gz` + `runtime.json`(程式每次啟動會依 config.json 重寫) |
| `nginx\` | nginx for Windows 1.30.5:`nginx.exe`、`docs\`(授權檔)、`conf\`(`mime.types` 等,**沒有** `nginx.conf`,由 §2 的步驟產生)、`SOURCE.txt` |
| `start-taidaflow.bat` / `.ps1` | 啟動(雙擊 `.bat`) |
| `stop-taidaflow.bat` / `.ps1` | 停止 TaidaFlow(不停 nginx) |
| `register-autostart.ps1` / `unregister-autostart.ps1` | 登入時自動啟動(工作排程器) |
| `scripts\install-nginx-config.ps1` | 產生 `nginx\conf\nginx.conf`(一次性步驟,§2) |
| `scripts\taidaflow-config.ps1` | 腳本共用的 config.json 讀取器 |
| `deploy\nginx\taidaflow.conf` | nginx 設定樣板 |
| `DEPLOY.md` | 本文件 |
| `VERSION.txt`、`MANIFEST.txt` | 版本資訊;每個檔的大小與 SHA-256 |

**不在打包裡**:`config.json`(第一次啟動時建立,§2)、`nginx\conf\nginx.conf`(§2 產生)、資料庫、設定 ini、log、測試或開發檔。

---

## 2. 第一次部署(用附的腳本,一步一步)

1. **複製**:把整個打包資料夾複製到正式機,改名放在 **`C:\TaidaFlow`**(以下的 `<安裝資料夾>`)。不要放在
   `C:\Program Files`(一般使用者不能寫入;config.json 與 nginx 設定要寫在這個資料夾裡)。路徑不要有空白、中文或
   `$ " ' { } ; #`。
2. **建立 config.json**(二選一):
   - 在 `<安裝資料夾>` 開 cmd,執行:
     ```bat
     cd /d C:\TaidaFlow
     C:\TaidaFlow\TaidaFlowApp.exe --write-default-config "C:\TaidaFlow\config.json"
     ```
     看到 `wrote the default configuration to ...` 即完成(只寫檔,不會開畫面、不會連設備;檔案已存在時印出
     `... already exists - not overwritten`,不覆寫,結果碼 3)。
   - 或第 5 步的第一次啟動會自動建立(但那次就會用預設值連設備與寫資料,所以建議先用上面的方式)。
3. **修改 config.json**:用記事本打開 `<安裝資料夾>\config.json`,依現場修改(欄位見 §6),通常要看的是:
   - `dataDir`:資料資料夾,預設 `C:\TaidaFlowData`(放在程式資料夾**外面**,更新程式不會碰到資料)。
     JSON 裡的反斜線要寫兩個:`"D:\\TaidaFlowData"`(或用正斜線 `"D:/TaidaFlowData"`)。
   - `devices`:5 台 ADAM 的 IP / port / unitId、MS300 的序列埠與通訊參數。
   - `nginx.port`(預設 80)、`modbusServer.port`(預設 502)等 port。
   存檔時編碼選 UTF-8。格式寫錯時程式會跳出錯誤視窗(含第幾行第幾欄)並結束,**不會**把檔案改回預設值。
4. **產生 nginx 設定**(一次;之後 config.json 或安裝資料夾改了才要再做):
   ```bat
   powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\scripts\install-nginx-config.ps1
   ```
   它讀 config.json,寫出 `<安裝資料夾>\nginx\conf\nginx.conf`(原本若有不是 TaidaFlow 的 `nginx.conf`,先備份成
   `nginx.conf.orig-<時間>`),再執行 `nginx -t`。要看到 `test is successful`,最後一行告訴你下一步。
   結果碼:`0` 完成;`2` 找不到檔案或 config.json 有誤;`3` `nginx -t` 失敗;`4` 已寫好,但 port 80 被別的程式占用
   (會列出占用者,不會關掉它,見 §10);`5` 網頁或匯出資料夾裡有捷徑連結 / junction(不寫)。
5. **啟動 nginx**(nginx 的標準做法):
   ```bat
   cd /d C:\TaidaFlow\nginx
   start nginx
   ```
   (可以跳過這一步:第 6 步的 `start-taidaflow.bat` 發現 nginx 沒執行時,會用同樣的方式啟動它。)
6. **啟動 TaidaFlow**:雙擊 `<安裝資料夾>\start-taidaflow.bat`(§3.1)。
7. **防火牆**:由管理員開 80 與 502(§7)。
8. **開機自動啟動**(需要時):§3.5。停電後要自動回到畫面,電腦要設定 Windows 自動登入(現場自行設定,§12)。
9. 確認:瀏覽器開 `http://127.0.0.1/`(或別台電腦開 `http://<IP>/`),畫面上方沒有紅色「離線」橫幅即同步成功。

第一次啟動後,資料資料夾裡會出現:

| 檔案 / 資料夾 | 內容 |
|---|---|
| `TaidaFlowSettings.ini` | 設定(`[Alarm] aiHighAlarmPercent` 等) |
| `settings.sqlite` | 感測器名稱、讀取頻率等設定(REST API 的 PUT 會改這裡,§8) |
| `device_info.ini` | 設備序號(`[device] sn`;沒有時建立,內容 `sn000000`) |
| `data\sensor_YYYYMM.sqlite` | 歷史資料與警報,每月一個檔 |
| `exports\` | 網頁匯出的 CSV(超過 20 個或 2 GB 時自動刪最舊的) |
| `logs\` | log 資料夾(config.json `log.dir`,預設 `logs` = 資料資料夾裡的 `logs\`):程式自己寫的 `taidaflow-<日期>.log`、`taidaflow-<日期>-full.log`,啟動 / 停止腳本的 `launcher-<日期>.log`,nginx 的 `nginx-access-<日期>.log`、`nginx-error.log`(§13) |
| `taidaflow-app.json` | 執行中的 TaidaFlow 身分(停止時用;停止後刪除) |
| `config.effective.json` | 只有啟動時用了單次覆寫參數(§3.1)才有:當次實際使用的完整設定 |

(`settings_schema.sql` / `data_schema.sql` 不需要:找不到時 app 用內建結構,log 有一行 `Schema file not found`,屬正常。)

---

## 3. 日常操作

### 3.1 啟動 TaidaFlow

**雙擊 `<安裝資料夾>\start-taidaflow.bat`。** 它會依序:

1. 讀 `config.json`(沒有就用預設值建立);格式錯誤 → 不啟動(結果碼 2)。
2. 檢查 TaidaFlow 是否已在執行、config.json 裡的 port 是否被別的程式占用(502、8124、8125、18125、18080,nginx 啟用時還有 80;
   80 若是 `nginx\` 的 nginx 本身在聽就沒問題)。
3. 刪除 log 資料夾裡過期的 `launcher-<日期>.log` 與 `nginx-access-<日期>.log`(超過 `log.quiet.keepDays` 天,預設 60;
   只刪完全符合這兩種檔名的檔,§13)。
4. nginx(`nginx.enabled` 為 true 時):確認 `nginx\conf\nginx.conf` 是**這個安裝資料夾與這份 config.json** 產生的
   ——不存在、資料夾搬過、config.json 改過 → 自動重新產生(舊檔留成 `nginx.conf.prev-<時間>`),nginx 執行中就 `nginx -s reload`;
   nginx 沒執行就以 `start nginx` 的方式啟動,已執行就不動它。nginx 起不來時 TaidaFlow 仍會啟動,網頁改走 app 自己的
   `:8124` / `:8125`(結果碼 8)。
5. 啟動 `TaidaFlowApp.exe`(工作目錄 = 資料資料夾;**沒有黑色的主控台視窗**,程式自己寫 log 檔),等 8124、8125 開始聽,
   顯示網址與結果碼,按任意鍵關閉這個黑色視窗(TaidaFlow 會繼續執行;關掉這個視窗也不會關掉 TaidaFlow)。

結果碼:`0` 成功;`4` 已經在執行或 port 被占用(**什麼都沒啟動**,也不會關掉別的程式);`6` app 沒起來(`launcher-<日期>.log`
會抄錄程式 log 的最後幾行;完整內容看 `taidaflow-<日期>-full.log`,§13);`8` app 已啟動但 nginx 沒起來;
`2` 打包資料夾不完整、資料夾不能寫或 config.json 有誤。

單次覆寫(只影響這一次,config.json 不改;寫成 `<資料資料夾>\config.effective.json` 交給程式):在 cmd 執行
`C:\TaidaFlow\start-taidaflow.bat -DataDir D:\Test`,可用的有 `-DataDir`、`-LogDir <log 資料夾>`(程式、nginx、腳本的 log
都改寫到那裡)、`-UseNginx` / `-NoNginx`、`-Port <nginx port>`、`-RestPort <port>`、`-Nginx <nginx.exe>`、`-Config <另一個 config.json>`。
(舊版的 `-AppLog quiet|full` 與 `-KeepLogDays` 已移除:quiet 與 full 兩種 log 由程式同時寫,保留天數在 config.json 的 `log`,§13。)

不要直接雙擊 `TaidaFlowApp.exe`:程式可以跑(會讀旁邊的 config.json),但不會做重複執行 / port 檢查,也不會帶起 nginx;
手動做法見 §5。

### 3.2 停止 TaidaFlow

**雙擊 `<安裝資料夾>\stop-taidaflow.bat`。** 它關閉 TaidaFlow 視窗(與手動按 X 相同,app 正常收尾),最多等 60 秒。
**nginx 不會被停**(它是獨立的程式)。

結果碼:`0` 已停止;`1` 沒有由 start-taidaflow 啟動的 app 在執行;`7` 60 秒內沒關閉(**不會強制結束**,請到畫面上手動關);
`2` config.json 有誤。真的關不掉時,管理員可以在 PowerShell 執行
`powershell -ExecutionPolicy Bypass -File C:\TaidaFlow\stop-taidaflow.ps1 -Force`
(強制結束:最後一筆資料庫寫入或進行中的匯出可能遺失,輸出維持最後狀態)。

### 3.3 nginx 的啟動 / 停止 / 套用設定

在 `<安裝資料夾>\nginx` 資料夾(cmd):

```bat
cd /d C:\TaidaFlow\nginx
start nginx          :: 啟動(沒有視窗,在背景執行)
nginx -s reload      :: 套用新的 nginx.conf(不中斷連線)
nginx -s quit        :: 正常停止(等傳送中的下載送完)
nginx -t             :: 只檢查設定
tasklist /fi "imagename eq nginx.exe"   :: 看有沒有在執行(一個 master + 一個 worker)
```

一定要**先 `cd` 到 nginx 資料夾**:nginx 用目前資料夾找 `conf\nginx.conf`、`logs\`、`temp\`。

### 3.4 什麼時候要重新產生 nginx 設定

改了 config.json 的 `nginx.port`、`rest.port`、`mirror.internalPort`、`dataDir`、`log.dir`、`nginx.exe`,搬了安裝資料夾,或更新成新的打包版本之後:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\scripts\install-nginx-config.ps1
cd /d C:\TaidaFlow\nginx
nginx -s reload
```

忘了也沒關係:`start-taidaflow.bat` 每次啟動都會比對 `nginx.conf` 檔頭記錄的安裝資料夾、config.json 路徑與內容(SHA-256),
不符就自動重新產生並 reload(`logs\launcher-<日期>.log` 有 `nginx.conf | ...` 的紀錄)。

### 3.5 登入時自動啟動

TaidaFlow 有操作畫面,所以採「**使用者登入時**」由工作排程器執行 `start-taidaflow.ps1`(不用 Windows 服務:服務跑在看不到
畫面的 session 0)。nginx 由 `start-taidaflow` 一起帶起來。

- **先乾跑**(不會註冊任何東西,只印出將要建立的排程與它會用的 config.json 設定):
  ```bat
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\register-autostart.ps1 -WhatIf
  ```
- 確認內容正確後,**由要自動登入的那個使用者**執行同一行但**拿掉 `-WhatIf`**。排程名稱 `TaidaFlow`:登入後 30 秒執行
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\TaidaFlow\start-taidaflow.ps1"`,
  只在使用者登入時執行、一般權限、已在執行就不再開第二個。設定一律從 config.json 讀,改 config.json 不用重新註冊。
- 移除:`powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\unregister-autostart.ps1`(先加 `-WhatIf` 看);
  只移除執行 start-taidaflow.ps1 的排程,不會關掉執行中的程式。
- 不用 PowerShell 的做法見 §5.8。
- 停電復電後要自動回到畫面,電腦必須**自動登入**該使用者(Windows 的自動登入設定,由現場自行設定;本專案的腳本不處理)。

### 3.6 備份

停止 TaidaFlow 後(或在不忙的時段)複製資料資料夾裡的 `data\`(所有 `sensor_YYYYMM.sqlite`)、`settings.sqlite`、
`TaidaFlowSettings.ini`、`device_info.ini`,以及 `<安裝資料夾>\config.json`。`exports\` 是可重新匯出的 CSV、`logs\` 是紀錄,
可選擇性備份。執行中直接複製 `.sqlite` 可能拿到寫到一半的狀態,正式備份請先停止。

---

## 4. 更新到新的打包版本

1. 雙擊 `stop-taidaflow.bat`;再到 `<安裝資料夾>\nginx` 執行 `nginx -s quit`(新版可能換了 nginx.exe)。
2. 把新打包資料夾裡的**所有檔案**複製到 `<安裝資料夾>`,覆蓋舊檔(新包沒有 `config.json`、沒有 `nginx\conf\nginx.conf`,
   所以現場的設定不會被蓋掉;資料資料夾在外面,不受影響)。
   想保留舊版以便退回時:先把 `<安裝資料夾>` 改名成 `C:\TaidaFlow.old`,把新包複製成 `C:\TaidaFlow`,再從舊的資料夾把
   `config.json` 複製回來。
3. 重新產生 nginx 設定(§3.4 的第一行)。沒做也可以:下一步啟動時會自動做。
4. 雙擊 `start-taidaflow.bat`。確認沒問題後再刪 `C:\TaidaFlow.old`(有的話)。
   自動啟動的排程指向 `C:\TaidaFlow\start-taidaflow.ps1`,路徑沒變就不用重新註冊。

---

## 5. 手動部署(完全不用 .ps1 腳本)

以下每一步都只用 cmd 與 Windows 內建工具。開發機上的步驟(5.1)需要 Qt;正式機上的步驟(5.2 起)不需要。

### 5.1 在開發機組出安裝資料夾

先照 `docs\BUILD.md` 編好桌面版(`build\desktop\TaidaFlowApp.exe`)與網頁版(`build\wasm-release\`)。在 cmd:

```bat
set "SRC=<repo>"
set "OUT=<輸出資料夾>\TaidaFlow"
set "QT=C:\Qt\6.8.3\msvc2022_64"
mkdir "%OUT%"
copy "%SRC%\build\desktop\TaidaFlowApp.exe" "%OUT%\"
set "PATH=%QT%\bin;%PATH%"
"%QT%\bin\windeployqt.exe" --release --no-compiler-runtime --no-translations --skip-plugin-types qmltooling,canbus --exclude-plugins qsqlmimer,qsqlodbc,qsqlpsql --qmldir "%SRC%\TaidaFlowContent" --qmldir "%SRC%\TaidaFlow" --qmldir "%SRC%\Dependencies" "%OUT%\TaidaFlowApp.exe"
```

- `windeployqt` 把 exe 需要的 Qt DLL、plugins 與 `qml\` 模組複製到 exe 旁邊(`--qmldir` 讓它掃描專案的 QML 找出要哪些 QML 模組)。
- **MSVC 執行環境**(app-local):從 Visual Studio 的 `VC\Redist\MSVC\<版本>\x64\Microsoft.VC145.CRT\`(版本要 ≥ 編譯用的工具組)
  複製所有 `.dll` 到 `%OUT%`:
  ```bat
  copy "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\<版本>\x64\Microsoft.VC145.CRT\*.dll" "%OUT%\"
  ```
- **網頁檔**:
  ```bat
  mkdir "%OUT%\web"
  for %f in (TaidaFlowApp.html TaidaFlowApp.js TaidaFlowApp.wasm qtloader.js qtlogo.svg) do copy "%SRC%\build\wasm-release\%f" "%OUT%\web\"
  ```
  (寫進 `.bat` 檔時 `%f` 要寫成 `%%f`。)
- **`.gz`(選用)**:沒有 `.gz` 網頁**也能正常運作**,只是第一次載入要傳約 34 MB 的 `.wasm`(有 `.gz` 約 12 MB)。要產生時,
  Windows 沒有內建 gzip 指令,可用任一方式:
  - 7-Zip:`"C:\Program Files\7-Zip\7z.exe" a -tgzip -mx=9 "%OUT%\web\TaidaFlowApp.wasm.gz" "%OUT%\web\TaidaFlowApp.wasm"`
    (`.js`、`.html` 同樣做);
  - 或在 PowerShell 視窗貼一行(不是 .ps1 檔):
    `$f='<輸出資料夾>\TaidaFlow\web\TaidaFlowApp.wasm'; $i=[IO.File]::OpenRead($f); $o=[IO.File]::Create("$f.gz"); $g=New-Object IO.Compression.GZipStream($o,[IO.Compression.CompressionLevel]::Optimal); $i.CopyTo($g); $g.Dispose(); $o.Dispose(); $i.Dispose()`
  - **`.gz` 一定要比原檔新或同時間**,而且原檔換新時 `.gz` 要一起重做(nginx 不比對時間,會送舊的 `.gz`)。
    不要為 `runtime.json` 做 `.gz`。
- **nginx**:把 nginx for Windows(nginx.org 的 zip,1.30.x)的 `nginx.exe`、`docs\` 與 `conf\`(**不要** `conf\nginx.conf`)
  複製到 `%OUT%\nginx\`,並建立 `%OUT%\nginx\logs` 與 `%OUT%\nginx\temp`:
  ```bat
  mkdir "%OUT%\nginx\conf" "%OUT%\nginx\logs" "%OUT%\nginx\temp"
  copy "C:\tools\nginx\nginx-1.30.5\nginx.exe" "%OUT%\nginx\"
  xcopy /e /i "C:\tools\nginx\nginx-1.30.5\docs" "%OUT%\nginx\docs"
  copy "C:\tools\nginx\nginx-1.30.5\conf\mime.types" "%OUT%\nginx\conf\"
  ```
  (`nginx\logs` 仍要有:nginx 在這裡放 `nginx.pid` 與它讀設定檔之前的訊息;請求紀錄與錯誤紀錄寫到 config.json 的 log 資料夾,§13。)
- 啟動 / 停止腳本(選用):`deploy\release\*.bat`、`*.ps1`、`scripts\install-nginx-config.ps1`、
  `scripts\taidaflow-config.ps1`、`deploy\nginx\taidaflow.conf` 照打包的位置複製(手動部署可以不用它們)。

把 `%OUT%` 整個複製到正式機的 `C:\TaidaFlow`。

### 5.2 建立 config.json

```bat
cd /d C:\TaidaFlow
C:\TaidaFlow\TaidaFlowApp.exe --write-default-config "C:\TaidaFlow\config.json"
notepad C:\TaidaFlow\config.json
```

依 §6 修改後存檔(UTF-8)。也可以直接照 §6 的範例自己打一份。

### 5.3 runtime.json

不用手動做:`TaidaFlowApp.exe` 每次啟動都會依 config.json 把 `web\runtime.json` 寫好
(nginx 啟用時 `{"mirrorPublicPort":<nginx.port>,"version":1}`,否則 `mirror.publicPort`)。

### 5.4 手動寫 nginx.conf

複製樣板再取代記號(或直接照下方完整範例打一份):

```bat
copy C:\TaidaFlow\deploy\nginx\taidaflow.conf C:\TaidaFlow\nginx\conf\nginx.conf
notepad C:\TaidaFlow\nginx\conf\nginx.conf
```

在記事本用「取代」(Ctrl+H)換掉六個記號,路徑一律用**正斜線 `/`**:

| 記號 | 換成 | 預設值的例子 |
|---|---|---|
| `@TAIDAFLOW_WEB_ROOT@` | 網頁資料夾**相對於 nginx 資料夾**的路徑(nginx 以自己的資料夾為基準解讀相對路徑,整個安裝資料夾搬家也有效) | `../web` |
| `@TAIDAFLOW_EXPORT_DIR@` | `<資料資料夾>\exports` 的完整路徑 | `C:/TaidaFlowData/exports` |
| `@TAIDAFLOW_LOG_DIR@` | log 資料夾的完整路徑(config.json 的 `log.dir`,相對路徑以資料資料夾為準;出現兩次)。這個資料夾要先存在(`mkdir`),nginx 不會自己建 | `C:/TaidaFlowData/logs` |
| `@TAIDAFLOW_NGINX_PORT@` | config.json 的 `nginx.port` | `80` |
| `@TAIDAFLOW_REST_PORT@` | config.json 的 `rest.port`(出現兩次) | `18080` |
| `@TAIDAFLOW_MIRROR_PORT@` | config.json 的 `mirror.internalPort` | `18125` |

完整範例(樣板去掉說明,記號用 `<...>` 表示;與 `install-nginx-config.ps1` 產生的內容相同):

```nginx
worker_processes  1;
error_log  "<log 資料夾>/nginx-error.log" warn;
pid        logs/nginx.pid;
events {
    worker_connections  1024;
}
http {
    server_tokens  off;
    map $time_iso8601 $taidaflow_log_date {
        "~^(?<taidaflow_ymd>[0-9]{4}-[0-9]{2}-[0-9]{2})T"  $taidaflow_ymd;
        default                                              "unknown-date";
    }
    access_log           "<log 資料夾>/nginx-access-$taidaflow_log_date.log";
    open_log_file_cache  max=4 inactive=60s valid=60s min_uses=1;
    client_body_temp_path  temp/client_body_temp;
    proxy_temp_path        temp/proxy_temp;
    fastcgi_temp_path      temp/fastcgi_temp;
    uwsgi_temp_path        temp/uwsgi_temp;
    scgi_temp_path         temp/scgi_temp;
    types {
        text/html               html;
        text/javascript         js mjs;
        application/wasm        wasm;
        text/css                css;
        application/json        json map;
        image/svg+xml           svg;
        image/png               png;
        image/jpeg              jpg;
        image/x-icon            ico;
        font/ttf                ttf;
        font/otf                otf;
        font/woff               woff;
        font/woff2              woff2;
    }
    default_type   application/octet-stream;
    charset        utf-8;
    charset_types  text/javascript text/css;
    etag               on;
    if_modified_since  before;
    gzip               off;
    gzip_static        on;
    gzip_vary          on;
    autoindex          off;
    absolute_redirect  off;
    keepalive_timeout  65;
    map $uri $taidaflow_export_name_ok {
        "~^/exports/[A-Za-z0-9_-]{1,40}_[0-9]{8}_[0-9]{6}\.csv$"  1;
        default                                                     0;
    }
    map $uri $taidaflow_cache_control {
        ~*\.html$  "no-cache";
        default    "no-cache";
    }
    server {
        listen       0.0.0.0:<nginx port>;
        server_name  _;
        root         "<網頁根目錄,例如 ../web>";
        location = / {
            add_header  Cache-Control "no-cache" always;
            return      302 /TaidaFlowApp.html;
        }
        location ^~ /exports/ {
            location ~ "^/exports/(?<taidaflow_export>[A-Za-z0-9_-]{1,40}_[0-9]{8}_[0-9]{6}\.csv)$" {
                alias  "<匯出資料夾>/$taidaflow_export";
                if ($taidaflow_export_name_ok = 0) {
                    return 404;
                }
                if (-d $request_filename) {
                    return 404;
                }
                types         { }
                default_type  "text/csv; charset=utf-8";
                charset       off;
                gzip          off;
                gzip_static   off;
                etag          on;
                max_ranges    1;
                add_header  Content-Disposition 'attachment; filename="$taidaflow_export"';
                add_header  Cache-Control "no-store" always;
                add_header  Access-Control-Allow-Origin "*" always;
            }
            return 404;
        }
        location ^~ /api/ {
            proxy_pass             http://127.0.0.1:<REST port>;
            proxy_http_version     1.1;
            proxy_set_header       Host $host;
            proxy_set_header       X-Real-IP $remote_addr;
            proxy_set_header       X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_connect_timeout  5s;
            proxy_send_timeout     60s;
            proxy_read_timeout     120s;
            client_max_body_size   1m;
            add_header             Cache-Control "no-store" always;
        }
        location = /api/ {
            proxy_pass             http://127.0.0.1:<REST port>/;
            proxy_http_version     1.1;
            proxy_set_header       Host $host;
            proxy_set_header       X-Real-IP $remote_addr;
            proxy_set_header       X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_connect_timeout  5s;
            proxy_read_timeout     30s;
            add_header             Cache-Control "no-store" always;
        }
        location = /mirror {
            proxy_pass             http://127.0.0.1:<mirror 內部 port>/mirror;
            proxy_http_version     1.1;
            proxy_set_header       Upgrade $http_upgrade;
            proxy_set_header       Connection "upgrade";
            proxy_set_header       Host $host;
            proxy_set_header       X-Real-IP $remote_addr;
            proxy_set_header       X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_connect_timeout  5s;
            proxy_read_timeout     3600s;
            proxy_send_timeout     3600s;
            proxy_buffering        off;
        }
        location = /runtime.json {
            gzip_static  off;
            etag         off;
            if_modified_since off;
            add_header   Cache-Control "no-store" always;
            add_header   Cross-Origin-Opener-Policy "same-origin" always;
            add_header   Cross-Origin-Embedder-Policy "require-corp" always;
            add_header   Cross-Origin-Resource-Policy "same-origin" always;
            try_files    $uri =404;
        }
        location ~* "^/(?:[^/.][^/]*/)*[^/.][^/]*\.(?:html|js|mjs|wasm|css|json|map|svg|png|jpg|ico|ttf|otf|woff|woff2)$" {
            add_header  Cache-Control $taidaflow_cache_control always;
            add_header  Cross-Origin-Opener-Policy "same-origin" always;
            add_header  Cross-Origin-Embedder-Policy "require-corp" always;
            add_header  Cross-Origin-Resource-Policy "same-origin" always;
            try_files   $uri =404;
        }
        location / {
            return 404;
        }
    }
}
```

檢查與啟動(log 資料夾不存在時 `nginx -t` 會因為開不了 `nginx-error.log` 而失敗,所以先建立):

```bat
mkdir C:\TaidaFlowData\logs
cd /d C:\TaidaFlow\nginx
nginx -t
start nginx
curl -I http://127.0.0.1/
```

`nginx -t` 要看到 `syntax is ok` 與 `test is successful`;`curl` 要看到 `HTTP/1.1 302` 與 `Location: /TaidaFlowApp.html`。
Windows 版 nginx 會跟隨網頁 / 匯出資料夾裡的捷徑連結或 junction,這兩個資料夾裡不要放這類東西。

### 5.5 手動啟動與停止 TaidaFlow

```bat
cd /d C:\TaidaFlow
start "" "C:\TaidaFlow\TaidaFlowApp.exe"
```

- 程式讀旁邊的 `config.json`,自己切換到 `dataDir`。一般使用者的環境沒有 `TAIDAFLOW_DEVICE_PROFILE`;若曾設定過,
  先 `set TAIDAFLOW_DEVICE_PROFILE=`(等號後面空白)清掉——正式機**一定**不能有這個測試用變數。
- log:程式自己寫到 config.json 的 log 資料夾(預設 `C:\TaidaFlowData\logs`,§13),不需要轉存輸出。
  不要設定 `QT_LOGGING_CONF` / `QT_LOGGING_RULES`(會讓 full log 少掉 info 訊息)。
- 確認:`netstat -ano | findstr LISTENING | findstr ":502 :8124 :8125 :18125 :18080"`。
- 停止:按視窗右上角的 X,或 `taskkill /IM TaidaFlowApp.exe`(**不要加 `/F`**:不加等於請視窗關閉,程式正常收尾)。

### 5.6 防火牆

見 §7 的 `netsh` 指令(由管理員執行)。

### 5.7 網頁與同步確認

- `curl http://127.0.0.1/runtime.json` → `{"mirrorPublicPort":80,"version":1}`
- `curl http://127.0.0.1/api/` → `{"status":"ok"}`
- 瀏覽器開 `http://<IP>/`,上方沒有紅色「離線」橫幅。

### 5.8 開機自動啟動(手動設定工作排程器)

開「工作排程器」(`taskschd.msc`)→ 建立工作(不是「基本工作」):

1. 一般:名稱 `TaidaFlow`;「只在使用者登入時執行」;不勾「以最高權限執行」。
2. 觸發程序:「登入時」,指定使用者,進階設定「延遲工作時間 30 秒」。
3. 動作(兩個,依序):
   - 啟動程式 `C:\TaidaFlow\nginx\nginx.exe`,**開始位置** `C:\TaidaFlow\nginx`(等於 `cd` 到那裡再 `start nginx`)。
   - 啟動程式 `C:\TaidaFlow\TaidaFlowApp.exe`,開始位置 `C:\TaidaFlow`。
   (或只放一個動作:`C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`,引數
   `-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\TaidaFlow\start-taidaflow.ps1"`,開始位置 `C:\TaidaFlow`——
   這就是 `register-autostart.ps1` 建立的內容。)
4. 條件:取消「只在使用 AC 電源時才啟動」。設定:「如果工作已在執行,不要啟動新的執行個體」。

停電復電後要自動回到畫面,需要 Windows 自動登入(現場設定)。

---

## 6. config.json 欄位

位置:`<安裝資料夾>\config.json`(與 `TaidaFlowApp.exe` 同一個資料夾;測試時可用環境變數 `TAIDAFLOW_CONFIG` 指到別的檔)。
UTF-8 文字檔,JSON 格式。

| 欄位 | 預設 | 說明 |
|---|---|---|
| `version` | `1` | 格式版本,固定 1 |
| `dataDir` | `C:\\TaidaFlowData` | 資料資料夾(設定 ini、每月資料庫、匯出、log)。程式啟動最早就建立並切換過去。相對路徑以 config.json 所在資料夾為準。無法建立時記 warning、維持目前的工作目錄 |
| `devices.adam6256` / `adam6217a` / `adam6217b` / `adam6224` / `adam6022` | `host` `192.168.1.201` ~ `.205`、`port` 502、`unitId` 1 | 5 台 ADAM(Modbus TCP)。`host` 必須是 IP 位址(不接受主機名稱);`unitId` 0~255 |
| `devices.ms300.serialPort` | `COM2` | MS300 變頻器的序列埠(Modbus RTU) |
| `devices.ms300.baudRate` | `9600` | 1200 / 2400 / 4800 / 9600 / 19200 / 38400 / 57600 / 115200 |
| `devices.ms300.dataBits` | `8` | 5~8 |
| `devices.ms300.parity` | `none` | `none` / `even` / `odd` / `space` / `mark` |
| `devices.ms300.stopBits` | `1` | 1 或 2 |
| `devices.ms300.unitId` | `1` | 站號 1~247 |
| `modbusServer.bind` / `port` / `unitId` | `0.0.0.0` / `502` / `1` | 給外部 HMI / SCADA 的 Modbus TCP 伺服器 |
| `http.bind` / `port` | `0.0.0.0` / `8124` | app 內建的網頁 + 下載(nginx 的備援) |
| `rest.bind` / `port` | `127.0.0.1` / `18080` | REST API(只限本機;區網經 nginx `/api/`)。改成非本機位址時 log 會警告 |
| `mirror.internalPort` | `18125` | 網頁同步伺服器,只綁 127.0.0.1(nginx `/mirror` 轉到這裡) |
| `mirror.publicBind` / `publicPort` | `0.0.0.0` / `8125` | app 內建的同步轉發(nginx 沒開時網頁連這裡) |
| `nginx.enabled` | `true` | `start-taidaflow` 是否帶起 nginx;也決定網頁的下載連結與同步 port:true → `nginx.port`,false → `http.port` / `mirror.publicPort` |
| `nginx.port` | `80` | nginx 監聽的 port |
| `nginx.exe` | `nginx\\nginx.exe` | nginx 程式位置(程式啟動 log 印出解析後的路徑;腳本用它找 nginx,`-Nginx` 可單次覆寫)。相對路徑以 **config.json 所在資料夾**為準(= 隨包的 `<安裝資料夾>\nginx\nginx.exe`);也可寫絕對路徑(例如另外安裝的 nginx) |
| `log.dir` | `logs` | log 資料夾。相對路徑以 **`dataDir`** 為準(預設 = `C:\TaidaFlowData\logs`);不存在時自動建立。程式的 log、腳本的 `launcher-<日期>.log`、nginx 的 log 都寫在這裡(§13) |
| `log.quiet.enabled` / `log.quiet.keepDays` | `true` / `60` | 精簡 log `taidaflow-<日期>.log`(只有 warning / critical / fatal)是否寫、保留幾天(1 以上;含今天)。腳本也用這個天數清理 `launcher-<日期>.log` 與 `nginx-access-<日期>.log` |
| `log.full.enabled` / `log.full.keepDays` | `true` / `7` | 完整 log `taidaflow-<日期>-full.log`(全部訊息,約每小時 15 MB、一天約 360 MB)是否寫、保留幾天(1 以上;含今天) |

規則:

- 檔案不存在 → 程式用上表預設值**建立**(UTF-8、縮排、無 BOM)。缺少的鍵 → 用預設值(不回寫檔案);不認得的鍵 → 忽略;
  型別或範圍錯(例如 port 超過 65535、IP 格式錯)→ 該項用預設值並記 warning。
- **JSON 格式錯誤**(少逗號、多逗號、引號不成對……)→ 程式顯示錯誤視窗(檔案、第幾行第幾欄、錯誤內容、
  「請修正 config.json 或刪除它讓程式重建預設值」)並結束(exit 2),**不覆寫**;`start-taidaflow` 也會拒絕啟動(結果碼 2)。
- Windows 路徑的反斜線在 JSON 要寫兩個(`"C:\\TaidaFlowData"`),或改用正斜線(`"C:/TaidaFlowData"`)。
- 程式的完整 log(`taidaflow-<日期>-full.log`,§13)在每次啟動時列出每一項的實際值與來源(file / default / environment),
  以及解析後的 `dataDir`、`nginx.exe`、`log.dir`。缺少或錯誤的值另有 info / warning 行(warning 也會進精簡 log)。
- config.json **讀不到**(JSON 格式錯誤、路徑是資料夾……)時,程式還不知道 `dataDir` 與 `log.dir`,這時的 log 寫到
  **`<config.json 所在資料夾>\logs`**(正式機 = `<安裝資料夾>\logs`),錯誤視窗與 `start-taidaflow` 的訊息會指出檔名(§13)。
- 不放在 config.json、固定在程式裡的值(決定事項):Modbus TCP 逾時 1000 ms / 重試 2 / 重連間隔 3000 ms;MS300 逾時 1000 ms /
  重試 1 / 輪詢 1000 ms。

範例(預設值;改設備位址與資料資料夾的現場通常只動這幾行):

```json
{
  "version": 1,
  "dataDir": "C:\\TaidaFlowData",
  "devices": {
    "adam6256": { "host": "192.168.1.201", "port": 502, "unitId": 1 },
    "adam6217a": { "host": "192.168.1.202", "port": 502, "unitId": 1 },
    "adam6217b": { "host": "192.168.1.203", "port": 502, "unitId": 1 },
    "adam6224": { "host": "192.168.1.204", "port": 502, "unitId": 1 },
    "adam6022": { "host": "192.168.1.205", "port": 502, "unitId": 1 },
    "ms300": { "serialPort": "COM2", "baudRate": 9600, "dataBits": 8, "parity": "none", "stopBits": 1, "unitId": 1 }
  },
  "modbusServer": { "bind": "0.0.0.0", "port": 502, "unitId": 1 },
  "http": { "bind": "0.0.0.0", "port": 8124 },
  "rest": { "bind": "127.0.0.1", "port": 18080 },
  "mirror": { "internalPort": 18125, "publicBind": "0.0.0.0", "publicPort": 8125 },
  "nginx": { "enabled": true, "port": 80, "exe": "nginx\\nginx.exe" },
  "log": { "dir": "logs", "quiet": { "enabled": true, "keepDays": 60 }, "full": { "enabled": true, "keepDays": 7 } }
}
```

(程式自己建立的檔案每個值一行、兩格縮排,內容相同。舊版建立、沒有 `nginx.exe` 或 `log` 的 config.json 照樣可以用:
缺少的鍵用上表預設值,log 只記一行 info,不會回寫檔案。)

---

## 7. port 與防火牆

| port / 位址(預設) | 程式 | 用途 | 防火牆 |
|---|---|---|---|
| `0.0.0.0:80` | nginx | 網頁 `http://<IP>/`、`/runtime.json`、網頁同步 `/mirror`(WebSocket)、CSV 下載 `/exports/`(續傳)、REST API `/api/` | **開** |
| `0.0.0.0:502` | TaidaFlowApp(Modbus TCP 伺服器) | 外部 HMI / SCADA 讀寫 | **開**(不限來源 IP) |
| `0.0.0.0:8124` | TaidaFlowApp(內建 HTTP) | 網頁與下載的備援(nginx 沒開時) | 不開 |
| `0.0.0.0:8125` | TaidaFlowApp(同步轉發) | 網頁同步的備援(nginx 沒開時) | 不開 |
| `127.0.0.1:18125` | TaidaFlowApp(同步伺服器) | 內部;nginx `/mirror` 與 8125 轉到這裡 | 不開(只綁本機) |
| `127.0.0.1:18080` | TaidaFlowApp(REST API) | 內部;nginx `/api/` 轉到這裡 | 不開(只綁本機) |
| 連出 ADAM 位址:502 | TaidaFlowApp(Modbus 用戶端) | 5 台 ADAM | —(連出) |
| MS300 序列埠 | TaidaFlowApp | 變頻器(Modbus RTU) | — |

正式機 Windows 防火牆**只開兩個輸入 TCP port:80 與 502**(Mango 決定;502 對所有來源開放)。其他 port 不對外。
本專案的腳本**不會**改防火牆;由管理員在「以系統管理員身分執行」的 cmd 執行:

```bat
netsh advfirewall firewall add rule name="TaidaFlow web (nginx 80)" dir=in action=allow protocol=TCP localport=80 profile=domain,private
netsh advfirewall firewall add rule name="TaidaFlow Modbus server (502)" dir=in action=allow protocol=TCP localport=502 profile=domain,private
```

(port 在 config.json 改過時用改過的值。第一次有程式在 `0.0.0.0` 開 port 時,Windows 可能跳出「允許存取」的詢問視窗,
由管理員決定。)網頁經 nginx 時,同步(`/mirror`)與下載都在 80,不需要開 8124 / 8125。

---

## 8. REST API

TaidaFlow 內建的 REST API 只在本機 `127.0.0.1:<rest.port>`(預設 18080)聽;區網的電腦一律經 nginx:`http://<IP>/api/...`。
**內網不做存取控管**:任何連得到 port 80 的人都能讀歷史資料,並用 PUT 改下表標「會改」的設定(決定事項,§12)。
回應都是 JSON,帶 `Access-Control-Allow-Origin: *`(每個 `/api/...` 都有 `OPTIONS` 預檢)。TaidaFlow 沒在執行時 nginx 回 `502`。

| 方法 | 網址 | 用途 | 參數 | 會改到什麼 |
|---|---|---|---|---|
| GET | `/api/` | 狀態 `{"status":"ok"}` | — | 不改 |
| GET | `/api/settings/sensors` | 感測器 key/名稱對照 | — | 不改 |
| PUT | `/api/settings/sensors` | 改感測器名稱 | body `[{"key":"s1","name":"..."}]`,沒列出的保留 | **會改** `settings.sqlite`(`sensor_config`;目前畫面不讀它) |
| GET | `/api/settings/frequency` | `{"read_frequency":1000}` | — | 不改 |
| PUT | `/api/settings/frequency` | 改讀取頻率設定值 | body `{"read_frequency":n}`,n > 0 | **會改** `settings.sqlite`(`app_settings`;目前沒有程式使用這個值,實際讀取週期不變) |
| GET | `/api/modbus/mode` | `{"mode":"network"}` | — | 不改 |
| PUT | `/api/modbus/mode` | 設定模式 | body `{"mode":"network"}` 或 `"standalone"` | **會改** 程式記憶體中的值(不影響 Modbus 連線;重啟後回 `network`) |
| GET | `/api/sensor/range` | 感測器歷史列(舊到新,**不分頁**) | `from`、`to`:epoch 秒 | 不改 |
| GET | `/api/sensor/rangeDateTime` | 同上 | `from`、`to`:`2026-09-28T10:00:00`(本機時間;`Z` 結尾 = UTC) | 不改 |
| GET | `/api/sensor/rangeDateTimePage` | 分頁版 | 同上 + `page`(預設 1)、`pageSize`(預設 200,最多 1000) | 不改 |
| GET | `/api/sensor/last` | 本月最新一列(沒有 → 404) | — | 不改 |
| GET | `/api/holding/range`、`/api/holding/rangeDateTime`、`/api/holding/rangeDateTimePage`、`/api/holding/last` | holding register 的同上查詢(目前不存 holding register,回空陣列 / 404) | 同 sensor | 不改 |
| GET | `/api/device/sn` | `{"sn":"sn000000"}` | — | `device_info.ini` 不存在時建立 |

範例(cmd):`curl http://127.0.0.1/api/settings/frequency`、
`curl -X PUT -H "Content-Type: application/json" -d "{\"read_frequency\":1000}" http://127.0.0.1/api/settings/frequency`。

**注意**:`/api/sensor/range`、`rangeDateTime` 一次回傳整個區間(每秒一列,一個月約 260 萬列),大區間會讓畫面停住很久;
大量資料請用 `rangeDateTimePage`。內部 port 要改(例如 18080 被別的程式占用):改 config.json 的 `rest.port`,再重新產生 nginx
設定(§3.4;`start-taidaflow` 也會自動做)。

---

## 9. 測試機(開發機接模擬器)與網頁版建置

開發機**沒有**真設備,用 `Adam60xxSimulator` 模擬 5 台 ADAM(`127.0.0.201~205:502`)。建置見 `docs\BUILD.md`;
以下在 `<repo>` 的 cmd 執行。

### 9.1 為什麼不能直接雙擊 `build\desktop\TaidaFlowApp.exe`

- 開發 build 的資料夾**沒有 Qt 的 DLL**(它們在 Qt 安裝資料夾的 `msvc2022_64\bin`),雙擊會出現「找不到 Qt6Core.dll」。PATH 上
  若有別的軟體附的 `Qt6Core.dll`,還可能載入**別的版本**的 Qt 而當掉。
- 就算 PATH 有 Qt:exe 旁邊沒有 config.json 時,程式會**在 `build\desktop` 旁建立正式機預設的 config.json**
  (資料寫到 `C:\TaidaFlowData`、連廠區位址 `192.168.1.201~205`、開 `COM2`)。開發機若正好接在工廠網段,就會**直接控制真設備**。
  所以開發機一律用 repo 的開發設定 `deploy\dev\config.dev.json`(以環境變數 `TAIDAFLOW_CONFIG` 指定),照下面的步驟或腳本(§9.3)。

### 9.2 手動步驟(cmd)

```bat
set "REPO=<repo>"
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
set "TAIDAFLOW_CONFIG=%REPO%\deploy\dev\config.dev.json"
```

**0. 先確認沒有人在用這些 port**(有輸出 = 有程式在用,先查清楚,不要關別人的程式):
`netstat -ano | findstr LISTENING | findstr /C:":80 " /C:":502 " /C:":8124 " /C:":8125 " /C:":18125 " /C:":18080 "`

**1. 模擬器**(一定要**先**開模擬器,再開 app):

```bat
mkdir "%REPO%\build\sim-cwd" 2>nul
cd /d "%REPO%\build\sim-cwd"
start "" "<模擬器資料夾>\Adam60xxSimulator.exe" --autostart
```

要看到 `127.0.0.201:502` ~ `127.0.0.205:502` 五行 `LISTENING`。

**1b. 安全探測(建議,這一步是 .ps1)**:`powershell -ExecutionPolicy Bypass -File "%REPO%\scripts\safety_probe.ps1" -DeviceProfile simulator -Reason "manual"`
→ 要看到 `verdict: SAFE`(exit 0)才繼續。

**2. TaidaFlowApp**:

```bat
set TAIDAFLOW_DEVICE_PROFILE=simulator
start "" "%REPO%\build\desktop\TaidaFlowApp.exe"
```

- `TAIDAFLOW_DEVICE_PROFILE=simulator`:ADAM 連線改到 `127.0.0.201~205`。**這一行不能漏**(或改用
  `set "TAIDAFLOW_CONFIG=%REPO%\deploy\dev\config.simulator.json"`,位址直接就是模擬器)。
- 程式切換到 config.dev.json 的 `dataDir` = `build\runtime-cwd`(已被 git 忽略)。

**3. nginx**:桌面版建置已產生 `build\desktop\nginx\`(含 `conf\nginx.conf`,BUILD.md §4.5),先部署網頁再啟動:

```bat
powershell -ExecutionPolicy Bypass -File "%REPO%\scripts\deploy-web.ps1"
cd /d "%REPO%\build\desktop\nginx"
start nginx
```

**4. 開網頁**:瀏覽器開 `http://127.0.0.1/`;備援 `http://127.0.0.1:8124/TaidaFlowApp.html`。不開瀏覽器的檢查:
`curl -I http://127.0.0.1/`、`curl http://127.0.0.1/runtime.json`、`curl http://127.0.0.1/api/`。

**5. 關閉**(nginx → app → 模擬器):

```bat
cd /d "%REPO%\build\desktop\nginx"
nginx -s quit
taskkill /IM TaidaFlowApp.exe
taskkill /IM Adam60xxSimulator.exe
```

等 5 秒後再跑步驟 0 的 netstat,沒有輸出 = 全部關乾淨。`taskkill` 不加 `/F`。

### 9.3 對應的 .ps1 腳本

| 手動步驟 | 腳本(repo 的 `scripts\`) |
|---|---|
| 1. 模擬器 | `powershell -ExecutionPolicy Bypass -File scripts\run-simulator.ps1` |
| 1b + 2. 探測 + app | `powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 -DeviceProfile simulator -Label "sim"`(`-Config` 換設定檔,預設 `deploy\dev\config.dev.json`;先安全探測,SAFE 才啟動) |
| 3. nginx | `build\desktop\nginx` 的 `start nginx`,或 `powershell -ExecutionPolicy Bypass -File scripts\nginx-start.ps1`(開發用、獨立的 `build\nginx` 前綴;兩者擇一) |
| 5. 關 nginx | `nginx -s quit`(在 `build\desktop\nginx`),或 `scripts\nginx-stop.ps1`(只停自己起的) |
| 網頁部署 | `powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1`(同時寫 `web\runtime.json`) |

### 9.4 打包與在開發機上驗證打包資料夾(開發人員)

```bat
scripts\build-desktop.bat
scripts\build-wasm.bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-release.ps1
powershell -ExecutionPolicy Bypass -File scripts\verify-release-package.ps1 -Package dist\TaidaFlow-<...>
```

`verify-release-package.ps1` 先跑安全探測,SAFE 才以「沒有 Qt 的 PATH」啟動打包資料夾,檢查第一次啟動建立 config.json、
`install-nginx-config.ps1` + `start nginx`、80 的網頁 / 同步 / 下載 / REST、搬移資料夾後自動重產 nginx 設定、壞掉的 config.json,
最後停止並還原成出貨狀態;不操作畫面。DLL 相依:`scripts\check-package-deps.ps1 -Package dist\TaidaFlow-<...>`。

### 9.5 網頁版

網頁是同一份 Qt 程式編成 WebAssembly,在瀏覽器裡跑,**本身不連設備**,資料全部經網頁同步(`/mirror`)與桌面程式同步。
建置:`scripts\build-wasm.bat`(BUILD.md §5);部署到 exe 旁的 `web\`:`scripts\deploy-web.ps1`(先清空舊的 `web\`,產生 `.gz`,
依 config.json 寫 `runtime.json`)。網頁載入後先讀同源的 `/runtime.json` 決定同步 port(讀不到時用 8125),再連
`ws://<載入網頁的主機>:<port>/mirror`;**畫面上方紅色「離線」橫幅 = 沒有同步**(§10)。瀏覽器以 ETag 重新驗證,重新整理就拿到新版。

---

## 10. 常見問題

**port 80 被占用(`start-taidaflow` 結果碼 4 / `install-nginx-config` 結果碼 4 / log 資料夾的 `nginx-error.log` 有 `bind() ... failed`)**:
畫面與 `logs\launcher-<日期>.log` 會列出占用者(port、pid、程式路徑),不會關掉它。
- 占用者是 **pid 4「System」**:Windows 的 HTTP.sys(IIS 的 World Wide Web 發佈服務、SQL Server Reporting Services、
  網頁部署代理程式、URL 保留等)。`netsh http show servicestate` 看是誰;由管理員停用那個服務,或改用別的 port
  (config.json 的 `nginx.port`,例如 8080 → 網址 `http://<IP>:8080/`,防火牆也要改開那個 port),改完做 §3.4。
- 占用者是另一個 `nginx.exe`(別的資料夾):那是另一套 nginx,要由它的管理者處理;兩者不能同時用同一個 port。
- `TaidaFlowApp is already running`:已經開著了(先 `stop-taidaflow` 或手動關)。
- port 502 被占用:可能是另一套 Modbus 軟體;兩者不能同時用 502。

**`nginx -t` 失敗**:看它印出的錯誤行。常見:手動改 `nginx.conf` 打錯字(重新執行 `install-nginx-config.ps1` 產生乾淨的);
路徑含 `$ " ' { } ; #`(換路徑);`conf\mime.types` 等檔不在(打包資料夾不完整,重新複製 `nginx\`)。
`install-nginx-config.ps1` 在 `nginx -t` 失敗時會保留寫好的檔方便檢查,結果碼 3。

**網頁打得開,但畫面上方一直是紅色「離線」(沒有即時資料)**:網頁同步沒連上。
- `curl http://127.0.0.1/runtime.json` 應是 `{"mirrorPublicPort":80,...}`(nginx 啟用時);如果是 8125,表示程式以 nginx 關閉的狀態
  啟動(例如 nginx 當時沒起來):檢查 nginx 後重新啟動 TaidaFlow。
- TaidaFlow 有沒有在執行(`/mirror` 由 nginx 轉給程式的 `127.0.0.1:18125`,程式沒開時 nginx 回 502)。
- `nginx.conf` 有沒有 `location = /mirror`(舊版設定沒有):執行 §3.4。
- 瀏覽器的開發者工具(F12)Console 會有 `/runtime.json not usable` 或 `WASM Mirror transport` 的訊息。
- 從別台電腦看:防火牆有沒有開 80。

**下載 CSV 失敗**:
- 網址的 port 應是 80(`nginx.enabled`);`http://<IP>/exports/<檔名>` 回 404:檔案已被清理(超過 20 個或 2 GB 會刪最舊的),
  或 nginx 設定的匯出資料夾與 config.json 的 `dataDir` 不一致(執行 §3.4)。
- 回 502 / 連不上:nginx 沒在執行(`start nginx`)。備援:`http://<IP>:8124/exports/<檔名>`(只在本機或防火牆有開時)。

**`start-taidaflow` 結果碼 2 且說 config.json is not valid JSON**:照訊息的行列修正(常見:少了逗號、多了最後一個逗號、路徑的
反斜線只寫一個),或刪掉 config.json 讓程式重建預設值(會失去現場的修改)。

**`start-taidaflow` 結果碼 6**:app 沒起來或 8124/8125 沒在聽。`launcher-<日期>.log` 抄了程式 log 的最後幾行;完整內容看
log 資料夾的 `taidaflow-<日期>-full.log`(結果碼是 2 = config.json 讀不到時,看 `<安裝資料夾>\logs`,§13)。

**log 資料夾裡沒有今天的 `taidaflow-<日期>.log`**:程式寫不進 log 資料夾(沒有權限、磁碟滿、`log.dir` 寫錯)。程式照常執行,
會每 60 秒與換日時重試;full 與 quiet 其中一個打得開時,另一個的錯誤會寫在打得開的那個檔。檢查 config.json 的 `log.dir`
與資料夾權限。

**找不到舊的 `taidaflow-20260928-093000.log` 這種檔名的 log**:那是舊版啟動腳本(每次啟動一個檔)的紀錄,新版不再產生,
舊檔也**不會**被自動刪除(§13);不需要時可以手動刪。

**`http://<IP>/api/...` 回 502**:TaidaFlow 沒在執行,或 REST API 沒起來(app log 有 `[REST] REST API NOT started`,
通常是 `rest.port` 被別的程式占用;改 config.json 的 `rest.port` 後做 §3.4)。

**畫面文字變成方框**:
- 桌面畫面:Windows 缺中文字型。確認 `C:\Windows\Fonts` 有「微軟正黑體」(`msjh.ttc`);沒有時由管理員在
  「設定 → 應用程式 → 選用功能」加入「中文(繁體)補充字型」或安裝繁體中文語言套件。
- 網頁:網頁內嵌的是字型**子集**,新加的中文字若不在子集裡會變方框 → 由開發人員重新產生字型並重新打包(BUILD.md §2.5)。

**log 裡很多 `Device is not connected` / `[MS300] ... serial device does not exist`**:連不到 ADAM 或沒有 MS300 的序列埠
(例如在辦公室測試)。正式機接好設備、config.json 位址正確後就不會一直出現。

**cmd 說 `'nginx' 不是內部或外部命令`(或 `TaidaFlowApp.exe` 找不到)**:先 `cd /d` 到那個程式的資料夾再打;
仍然找不到時(少數電腦設定了環境變數 `NoDefaultCurrentDirectoryInExePath`,cmd 就不在目前資料夾找程式),改打
`.\nginx.exe`、`start .\nginx.exe` 或完整路徑(例如 `C:\TaidaFlow\TaidaFlowApp.exe`)。

**雙擊 .bat 出現「無法載入,因為這個系統上已停用指令碼執行」**:.bat 已用 `-ExecutionPolicy Bypass`;
若仍被擋,是公司群組原則(GPO)禁止,請 IT 放行 `<安裝資料夾>` 的腳本,或照 §5 手動操作。

---

## 11. 開發機腳本與正式機腳本的差異(不可混用)

| | 開發機(repo `scripts\`) | 正式機(打包資料夾) |
|---|---|---|
| 啟動 | `run-desktop.ps1`:**先跑安全探測**(config.json 的設備位址連得到、設定的序列埠存在、或 Modbus 伺服器 port 被占用就**拒絕**) | `start-taidaflow.bat/.ps1`:**不做安全探測**(現場本來就要連真設備),只檢查重複執行與 port |
| 設定檔 | `deploy\dev\config.dev.json`(`TAIDAFLOW_CONFIG`),`-Config` 可換 | `<安裝資料夾>\config.json` |
| Qt | 從 Qt 安裝資料夾(腳本加到 PATH) | 打包資料夾自帶;PATH 中含 Qt 的資料夾會被移除 |
| 工作目錄 | config.dev.json 的 `dataDir` = `build\runtime-cwd` | config.json 的 `dataDir`(預設 `C:\TaidaFlowData`) |
| 設備 | 可用 `-DeviceProfile simulator` 或 `config.simulator.json` 改連模擬器 | **一律清除** `TAIDAFLOW_DEVICE_PROFILE`(不能進測試模式) |
| nginx | `build\desktop\nginx` 的 `start nginx`,或 `nginx-start.ps1`(`build\nginx` 前綴) | `<安裝資料夾>\nginx` 的 `start nginx`(`start-taidaflow` 會帶起) |
| log | 程式自己寫到 config.dev.json 的 log 資料夾(`build\runtime-cwd\logs`);`run-desktop.ps1` 另外把輸出存到 `build\runtime-logs\`(開發用) | 程式自己寫到 config.json 的 `log.dir`(預設 `<資料資料夾>\logs\`):quiet 保留 60 天、full 保留 7 天;`launcher-<日期>.log`、nginx log 在同一個資料夾(§13) |

在正式機跑開發機腳本:安全探測一定判定不安全,app 不會啟動。在開發機跑正式機腳本:**沒有保護**,
若開發機連得到設備位址就會控制真設備——開發機請只用 `scripts\` 的腳本(或 §9.2 的手動步驟)。

---

## 12. 決定事項

原本「仍需 Mango 決定的事項」,2026-09-28 已決定:

1. **資料資料夾**:預設 `C:\TaidaFlowData`(config.json `dataDir`,現場可改)。**已決定。**
2. **設定檔**:所有現場設定放 `config.json`(取代舊的站台批次檔);沒有時自動建立預設檔;更新版本不覆蓋。**已決定。**
3. **設備位址**:ADAM 位址、MS300 序列埠參數都在 config.json,現場可改(不必改程式)。Modbus / MS300 的逾時、重試、輪詢間隔
   維持寫在程式裡,不放 config.json。**已決定。**
4. **nginx**:隨打包附上(`<安裝資料夾>\nginx`,1.30.5),以 nginx 標準方式 `start nginx` 啟動;設定由
   `install-nginx-config.ps1` 產生、隨設定變更自動重產。**已決定。**
5. **VC++ 執行環境**:維持隨程式放 DLL(app-local),現場不安裝 `vc_redist`。**已決定。**
6. **開機自動啟動與自動登入**:登入時由工作排程器啟動;Windows 自動登入由現場自行設定,本專案的腳本不處理。**已決定。**
7. **防火牆**:只開 80(nginx:網頁、同步、下載、REST)與 502(Modbus,不限來源)。**已決定。**
8. **存取控管**:80(含 REST API 的 PUT)與 502 不做登入或來源限制(內網)。任何連得到的人都能操作(含急停)。**已決定。**
9. **log**:程式自己寫檔(不再由啟動腳本轉存輸出);quiet(警告與錯誤)保留 60 天 + full(全部訊息)保留 7 天,兩者預設都開、
   檔名帶日期、換日時清理;log 資料夾預設也在 config.json(`log.dir`,預設 `C:\TaidaFlowData\logs`);config.json 讀不到時也要有
   log(寫到 config.json 旁的 `logs\`);不刪減原本的 log 訊息、舊版的 log 檔不刪。**已決定,已實作(w2-064、w2-065,§13)。**
10. **現場電腦設定**(睡眠、Windows Update 自動重開機時段、螢幕保護)與**備份**的頻率與位置(§3.6):由 IT / 現場決定。

---

## 13. log(紀錄檔)

TaidaFlow 的 log **由程式自己寫檔**(w2-064),啟動腳本不再把程式的輸出轉存成檔案(w2-065)。所有 log 都在同一個
**log 資料夾**:config.json 的 `log.dir`,相對路徑以 `dataDir` 為準,預設 `logs` → **`C:\TaidaFlowData\logs`**;不存在時程式與腳本會建立。

### 13.1 檔案一覽

| 檔名 | 誰寫 | 內容 | 保留(預設) | 誰清理 |
|---|---|---|---|---|
| `taidaflow-YYYY-MM-DD.log` | 程式 | 精簡(quiet):只有 warning / critical / fatal | `log.quiet.keepDays` = 60 天 | 程式 |
| `taidaflow-YYYY-MM-DD-full.log` | 程式 | 完整(full):全部訊息(debug / info 以上),約每小時 15 MB、一天約 360 MB | `log.full.keepDays` = 7 天(約 2.5 GB) | 程式 |
| `launcher-YYYY-MM-DD.log` | `start-taidaflow` / `stop-taidaflow` | 每次啟動 / 停止的步驟、使用的設定、port 檢查、nginx、結果碼;程式在啟動過程中結束時,抄錄它最後的 log 行 | `log.quiet.keepDays` = 60 天 | `start-taidaflow` |
| `nginx-access-YYYY-MM-DD.log` | nginx | 每個 HTTP 請求一行(網頁、`/runtime.json`、`/api/`、`/exports/` 下載、`/mirror` 連線) | `log.quiet.keepDays` = 60 天 | `start-taidaflow` |
| `nginx-error.log` | nginx | nginx 的警告與錯誤(`warn` 以上,量很少) | 不清理(一個固定檔名) | —(需要時手動刪) |

- 日期 `YYYY-MM-DD` 是本機時間的日期。quiet 與 full **預設同時寫**,各自可在 config.json 用 `enabled: false` 關掉;
  關掉的那種仍會依它自己的 `keepDays` 清掉過期的舊檔。今天的 quiet 檔在啟動時就會建立(即使還沒有警告,是空檔)。
- 每一行的格式:`2026-09-28 14:03:05.007 [info] [Config] ...`(時間到毫秒、層級、訊息;UTF-8、CRLF 換行)。
  原本程式印出的訊息內容**一字不刪**,只是在前面加上時間與層級。
- 程式仍同時把訊息送到除錯輸出(開發時在 Qt Creator 看得到)。
- `nginx\logs\`(nginx 資料夾裡)只剩 `nginx.pid` 與 nginx 讀到設定檔之前的訊息(例如 `nginx -t`、`nginx -s reload` 本身的錯誤),不會變大。

### 13.2 保留天數與清理規則

- 「保留 N 天」= 含今天在內的 N 天:`keepDays` 60 → 2026-09-28 時保留 2026-07-31 ~ 2026-09-28 的檔;7 → 保留 09-22 起;1 → 只留今天。
  日期比今天晚的檔(例如曾把系統時間調回去)不刪。
- 程式在**啟動時**與**換日時**(跨過午夜,或系統日期被調整)清理自己的 `taidaflow-*.log`,換日時也換到新日期的檔。
- `start-taidaflow` 在**每次啟動時**清理 `launcher-*.log` 與 `nginx-access-*.log`(天數 = `log.quiet.keepDays`)。
- **只刪檔名完全符合上面格式的檔**(大小寫、數字、合法日期都要符合)。log 資料夾裡的其他檔——舊版啟動腳本的
  `taidaflow-yyyyMMdd-HHmmss.log` / `.stdout`、舊版的 `launcher.log`、`nginx-error.log`、自己放的筆記、子資料夾——**一律不動**
  (Mango:不要刪掉原本的資料內容)。舊版的檔不需要時請手動刪除。
- 刪不掉的檔(唯讀、被別的程式開著)記一行 warning,下次清理再試,不會影響程式執行。

### 13.3 寫不進去的時候

- log 資料夾不能寫(沒有權限、磁碟滿、`log.dir` 寫錯):**程式照常執行**,每次出問題只在 stderr 記一次警告,每 60 秒與換日時重試;
  quiet / full 其中一個還能寫時,另一個的錯誤會寫在能寫的那個檔。
- **config.json 讀不到**(JSON 格式錯誤、路徑是資料夾……):程式還不知道 `dataDir` / `log.dir`,就寫到
  **`<config.json 所在資料夾>\logs`**(正式機 = `<安裝資料夾>\logs`,同樣的檔名與保留天數),錯誤視窗最後一行寫出 log 檔的位置,
  程式以結果碼 2 結束,config.json 不會被改。`start-taidaflow` 自己先檢查 config.json,格式錯誤時根本不啟動程式(結果碼 2,
  訊息在畫面上);只有直接執行 `TaidaFlowApp.exe` 時才會產生這個備援 log。
- 程式在啟動過程中就結束(結果碼 6):`start-taidaflow` 把程式寫到 stderr / stdout 的內容(通常沒有)與 log 檔最後幾行抄進 `launcher-<日期>.log`。

### 13.4 nginx 的 log 與長時間不重啟

- `nginx-access-<日期>.log` 的檔名由**每個請求當下的日期**決定:過了午夜,下一個請求就寫進新日期的檔,**不需要重啟或 reload nginx**。
- 但**清理**只在 `start-taidaflow` 啟動時做:TaidaFlow / nginx 連續好幾個月不重啟時,舊的 `nginx-access-*.log` 會一直留著。
  一般使用量(網頁載入與下載,同步走一條長連線)一天通常只有幾十 KB ~ 數 MB,影響不大;需要時可以:
  - 定期(例如每月)重新啟動一次 TaidaFlow(`stop-taidaflow` → `start-taidaflow`),啟動時就會清理;或
  - 手動刪除過期的 `nginx-access-<日期>.log`(nginx 執行中也可以刪不是今天的檔);或
  - 需要自動清理時,由現場 IT 另設排程(本專案的腳本不註冊排程)。
- `nginx-error.log` 不會自動清理;只記 `warn` 以上,正常情況下很小。要清空:停止 nginx(`nginx -s quit`)後刪除,再 `start nginx`。

### 13.5 啟動腳本的變更(w2-065)

- 移除:把程式輸出轉存成 `taidaflow-<日期-時間>.log`、`logging\quiet.ini` 與 `QT_LOGGING_CONF`(它會在程式寫檔之前就把 info 訊息濾掉,
  full log 就會沒有 info)、`-AppLog quiet|full`、`-KeepLogDays`、以修改時間刪除舊 app log 的清理。
- `start-taidaflow` 會清除環境變數 `QT_LOGGING_CONF`、`QT_LOGGING_RULES`、`QT_FORCE_STDERR_LOGGING`、`QT_ASSUME_STDERR_HAS_CONSOLE`,
  讓程式自己決定每種 log 寫什麼。
- 程式以**沒有主控台視窗**的方式啟動(舊版會多一個黑色視窗,關掉它會連帶結束程式)。
- `launcher.log` 改為 `launcher-<日期>.log`,與程式的 log 放在同一個 log 資料夾;舊的 `launcher.log` 保留不動。
- `-LogDir <資料夾>`:單次覆寫 `log.dir`,程式、nginx、腳本的 log 都寫到那裡(寫在 `config.effective.json` 交給程式與 nginx 設定產生器;
  `stop-taidaflow` 由 `taidaflow-app.json` 得知這個資料夾)。平常請改 config.json 的 `log.dir`。
