# TaidaFlow WebAssembly 整合報告

施作流程、問題報告與維護方建議

| 項目 | 內容 |
|---|---|
| 專案 | TaidaFlow(https://github.com/EllieFeng8/TaidaFlow.git) |
| 整合基準 | 分支 `core`,commit `dc91f01`(「更新History」) |
| 使用套件 | wasm-mirror integration pack **1.0.1**(wire protocol 3) |
| 建置環境 | Qt 6.8.3(msvc2022_64 / wasm_singlethread)、emsdk 3.1.56、MSVC 2022 |
| 日期 | 2026-09-24 |
| 驗收 | 通過。PM 親自重跑建置、檢查腳本與端到端同步測試(見 §3) |

---

## 1. 結論

`core` 分支已加上 WebAssembly 版本:瀏覽器網頁和桌面程式同步同一份 `TaidaFlowProxy`
狀態,兩邊的修改會互相看到。

- **後端只在桌面版**:Modbus、MS300、REST、SQLite 全部只編進桌面版。網頁版只有 UI
  和 Proxy,不會也無法直接連任何設備。
- **斷線時網頁鎖住操作**:桌面斷線時,網頁顯示紅色「離線」橫幅,所有操作控制項停用,
  重連後自動恢復。這是本輪新增的安全措施,原因見 §4.2。
- **桌面版外觀不變**:三個頁面和改動前逐像素比對,差異為 0。
- **pack 原封不動**:pack 1.0.1 整包放入,25 個檔案與官方來源逐位元相同。

---

## 2. 施作流程

### 2.1 歷程

| 輪次 | 基準 | pack | 結果 |
|---|---|---|---|
| 第一輪 | `main` d2852fe | 1.0.0(repo 內附) | 通過。但發現 pack 1.0.0 的問題(§4.1),保存於分支 `backup/wasm-v2-pack1.0.0`,不採用 |
| 第二輪 | `main` d2852fe → 306dc1d | 1.0.1 | 依指示改拉最新版,中途暫停作廢(半成品在 stash) |
| **本輪** | **`core` dc91f01** | **1.0.1** | **通過(本文件)** |

### 2.2 本輪步驟

整合點原則上只有四個:**Proxy、CMake、main.cpp、HTTP server**。超出這四個的步驟列於 §2.3。

**步驟 0:安全確認(每次啟動桌面程式前)**
`core` 的後端會連 5 台 ADAM(程式寫死 192.168.1.201~205:502)和 COM2 的 MS300,
測試時點「緊急停止」會真的送 Modbus 寫入。因此每次啟動前都先執行
`scripts/safety_probe.ps1`:確認 5 個 IP 都連不到、本機沒有序列埠、port 502 沒有其他
程式在用,任一條件不符就不啟動。本輪共啟動 9 次(實作 6 次、PM 複驗 3 次),每次結果都是 SAFE,紀錄在
`docs/evidence/wasm-v4/safety-probe.log`。桌面程式一律從 `build/runtime-cwd/` 啟動,
設定檔和 SQLite 不會寫進原始碼目錄。

**步驟 1:放入 pack**
刪除舊的 `integration-pack/wasm-mirror/`(1.0.0),整包複製 1.0.1。
用 `scripts/verify_pack.py` 驗證:MANIFEST 24/24,25 個檔和官方來源 SHA-256 全部相同。
另新增 `.gitattributes`,讓 git 不轉換 pack 的換行符(否則 `core.autocrlf=true`
會在 checkout 時改掉 pack 檔案,hash 全部失效)。

**步驟 2:CMake**
- 根 `CMakeLists.txt`:先判斷 `EMSCRIPTEN`,再判斷 `WIN32`。SerialBus、SerialPort、
  Sql、HttpServer、Concurrent 只在桌面版 `find_package`。
- `Core/CMakeLists.txt`:網頁版只編 `TaidaFlowProxy.h`;7 個後端 `.cpp` 和上述 Qt 模組
  只在桌面版編譯與連結。
- Proxy 註冊(`wasm_mirror_register_proxy` / `finalize`)放在擁有 Proxy 標頭的 `Core` target。

**步驟 3:main.cpp**
- 桌面版照舊:`Core::instance().init()`,取 `core.m_proxy`。
- 網頁版:直接建立 `TaidaFlowProxy` 當鏡像(建構子沒有硬體副作用)。
- `Td` 改註冊到 URI **`TaidaFlowBackend`**。原本的 `"Core"` 和 `qt_add_qml_module(URI Core)`
  撞名,桌面版看不出問題,網頁版整個 QML 會載入失敗(pack 文件已警告)。
- 網頁版在載入 QML 前先把連線狀態設為「連線中」,並安裝 `transportStateHandler`。
- 載入內嵌中文字型;字型替代鏈只在網頁版建立。

**步驟 4:Proxy**
新增 `transportReady`、`transportMessage` 兩個屬性,都設 `STORED false`(不參與同步,
網頁的離線狀態不會回寫到桌面)。預設值是「已連線」,所以桌面版永遠不會出現離線畫面。

**步驟 5:HTTP server**
`scripts/serve_wasm.py` 在 127.0.0.1:8123 提供網頁,帶必要的 COOP/COEP 標頭。

**步驟 6:QML(偏離,見 §2.3)**
三個頁面的 import 由 `Core 1.0` 改為 `TaidaFlowBackend 1.0`。加入離線橫幅,並讓
所有操作控制項綁定連線狀態:變頻器復歸、緊急停止、M1~M4、泵浦開關與頻率、二通閥、
歷史頁翻頁。離線時已開啟的對話框也會自動關閉。

**步驟 7:中文字型(偏離)**
瀏覽器裡的 Qt 拿不到系統字型,中文會變成方框。從 Noto Sans TC(OFL-1.1)做出只含
本專案用字的子集:490 字(中文 385 字),Regular + Bold 共 370 KB。

**步驟 8:驗證**(見 §3)

### 2.3 超出四個整合點的步驟(偏離清單)

| 編號 | 內容 | 原因 |
|---|---|---|
| DV-1 | QML import 改為 `TaidaFlowBackend 1.0`(Main、AlarmPage、HistoryPage、TopNav) | 依文件解 URI 撞名的連帶修改 |
| DV-2 | 離線橫幅與控制項停用(TopNav、Main、MotorIcon、HistoryPage) | 斷線安全措施(§4.2) |
| DV-3 | 中文字型子集(`App/embeddedfonts.*`、`App/fonts/`、`scripts/make_font_subset.py`) | 網頁無系統字型;pack 文件列為宿主自理 |
| DV-4 | `App/e2epvdriver.h` | 測試專用的 PV 數值注入(只編進桌面版,需設環境變數才啟用,只接受 `*Pv` 屬性) |
| DV-5 | `scripts/` 建置、驗證、安全探測工具 | 可重現驗證 |
| DV-6 | `.gitattributes` | 保護 pack 位元組不被換行轉換 |
| DV-7 | README 補完整說明,並由 UTF-16LE(無 BOM)轉為 UTF-8(無 BOM、LF 換行);轉碼在 w2-027-fix1 完成,之前的提交(65b019a)仍是 UTF-16LE | 文件 |
| DV-8 | `CMakePresets.json` | `core` 的 `.gitignore` 排除了它,commit 時需 `git add -f` |
| DV-9 | `docs/evidence/wasm-v4/`(47 個證據檔)、`docs/evidence/wasm-v4-csv/`(CSV 下載) | 驗證紀錄 |
| DV-10 | `saveHistoryCsv` 網頁分支與 `HistoryPage.qml` 下載訊息(§4.4) | 網頁版下載檔案 |

---

## 3. 驗證結果

以下都以程式的 exit code 判定,並由 PM 在交付後親自重跑。

| 項目 | 結果 |
|---|---|
| pack 完整性 | MANIFEST 24/24;25 檔與官方來源逐位元相同 |
| pack 內附測試 | Engine、Runtime 兩組全部通過 |
| 桌面版 fresh build | exit 0 |
| 網頁版 fresh build | exit 0,`TaidaFlowApp.wasm` 33,657,326 bytes(約 32 MiB,含 §4.4 的 CSV 修正) |
| 網頁版不含後端 | 7 個後端 `.cpp`、5 個後端 Qt 模組、後端字串皆為 0 |
| VERSION 檔攔截檢查 | 桌面 0、網頁 0 |
| 繞道設定 | 無 `NO_PLUGIN`;無額外的 `CMAKE_INCLUDE_CURRENT_DIR` 設定 |
| 字型涵蓋 | 缺字 0 |
| 桌面外觀 | 主頁、警報、歷史三頁像素差異皆 0 |
| 桌面啟動 | 後端啟動、mirror 端點正常、關閉後無殘留連接埠 |
| 雙向同步 | 數值(M1、M2、M3)與開關(緊急停止、二通閥)雙向皆通過 |
| 唯讀 PV | 溫度、壓力、馬達/二通閥狀態回授,桌面 → 網頁單向通過 |
| 斷線 / 重連 | 斷線後網頁顯示離線橫幅;約 5 秒自動重連並套用最新狀態 |
| 離線操作 | 離線時點緊急停止、M1、二通閥、復歸都沒有反應,重連後桌面確認未收到任何指令 |
| 設備安全 | 9 次啟動前探測皆 SAFE;log 顯示沒有任何 Modbus 封包送達設備 |

PM 另外獨立抽測(2026-09-24 12:07~12:10,每次啟動前探測皆 SAFE):網頁連線正常 →
關閉桌面 → 網頁立即出現離線橫幅 → 點擊緊急停止與 M1 皆無反應 → 重啟桌面 → 網頁自動重連、
橫幅消失、緊急停止仍為 off;桌面 log 確認這段期間緊急停止指令 0 筆、Modbus 寫入 0 筆。

> 注意:抽測期間本機 Wi‑Fi 從 192.168.0.x 換到 192.168.168.x。探測結果仍是 SAFE,
> 但也說明測試中途網路可能改變,而 Core 每 3 秒會重連設備。在可能連到真實設備的
> 網路上,不應以「寫死真實 IP」的版本做開發測試(見 §5 建議 8)。

---

## 4. 問題報告

### 4.1 pack 1.0.0 的問題(升級到 1.0.1 後已解決)

這些是第一輪用 repo 內附的 1.0.0 時發現的,也是改用 1.0.1 的原因。

**(1) `VERSION` 檔會取代 C++ 標準標頭 `<version>`,而且不會報錯**
- 現象:pack 1.0.0 根目錄有一個名為 `VERSION` 的檔案。QDS 專案預設
  `CMAKE_INCLUDE_CURRENT_DIR ON`,會把 pack 根目錄加進 include 路徑;Windows 檔名不分
  大小寫,於是 Qt 標頭 `qcompilerdetection.h → qtconfiginclude.h` 中的
  `#include <version>` 讀到的是這個檔案,而不是 C++ 標準標頭。
- 證據:在原始的 d2852fe 重現,3 個 pack 編譯單元命中。
- 影響:Qt 靠 `<version>` 判斷編譯器支援哪些 C++ 功能(QtCore 標頭中約 139 處判斷)。
  該檔全是註解,所以不會編譯錯誤;目前 C++17 / MSVC 下沒觀察到實際損害,但屬於
  完全無聲的潛伏風險。
- 歷程:1.0.0 官方 zip 本身沒有防護。維護方在 78023c3(8/17)曾在 pack 內自行加上
  `set(CMAKE_INCLUDE_CURRENT_DIR OFF)`,a0babbe(8/20)又刪除,問題才重新出現。
- 狀態:1.0.1 已根治(改名為 `VERSION.txt`,並內建上述防護)。

**(2) pack 的 MANIFEST 驗證失敗**
- 原因:不是檔案內容毀損。上述「補上防護又刪除」之後,pack 的 `CMakeLists.txt` 比官方
  版本少了最後一個空行,hash 就對不上了。
- 狀態:1.0.1 整包替換後 24/24 相符。

**(3) WASM 版的 QML 模組 URI 撞名**
- `qmlRegisterSingletonInstance("Core", …)` 和 `qt_add_qml_module(URI Core)` 同名。
  桌面版正常,網頁版整個 QML 載入失敗。
- 狀態:依 1.0.1 文件,把 `Td` 移到獨立 URI `TaidaFlowBackend`。

**(4) WASM 沒有部分 Qt 模組**
- wasm_singlethread 沒有 Qt Concurrent、SerialBus、SerialPort。
- 狀態:依 1.0.1 文件,這些只在桌面版引用。

### 4.2 安全問題:斷線時網頁仍可操作(已修正)

- 現象:第一輪(沒有離線提示)時,桌面關閉後,網頁上的「緊急停止」仍可切成紅色 ON,
  畫面沒有任何斷線提示,但這個指令根本沒有送出。操作員可能以為已經急停。
- 修正:依 pack 文件 §9 實作離線提示,離線時停用所有操作控制項(§2.2 步驟 4、6)。
- 驗證:離線時點擊緊急停止等控制項都沒有反應;重連後桌面 log 確認沒有收到任何指令。

### 4.3 產品面觀察(只量測,未修改)

1. **網頁端可以解除緊急停止**。緊急停止是雙向同步的屬性,網頁上的任何人都能把它關掉。
2. **`saveHistoryCsv` 在網頁版會在瀏覽器本機執行**(已修正,見 §4.4)。它不經過同步;原本
   網頁版會呼叫 `QFileDialog::getSaveFileName` + `QFile`,檔案只會寫進瀏覽器的虛擬檔案系統。
3. **歷史頁頁碼是共享狀態**。`historyCurrentPage` 會同步,而且它一改就觸發 SQL 重新查詢。
   所以有人在網頁翻頁,桌面和其他網頁也會一起翻。本輪只有 1 頁資料,無法實測。
4. **Modbus server 綁在 0.0.0.0:502**,外部網卡看得到,也會和本機其他 502 服務衝突。
5. **設備位址寫死在程式裡**(ADAM 192.168.1.201~205、MS300 COM2),沒辦法改接模擬器
   (`Adam60xxSimulator` 用的是 127.0.0.201~205)。
6. **找不到 `data_schema.sql`**:Core 會到「目前工作目錄」找這個檔案,啟動 log 有多行
   `Schema file not found`。
7. **沒有設備時 log 量很大**:約 4 分鐘產生 3,590 行 `request was not sent`;MS300 每秒
   重試約 20 行。
8. **Proxy 建構子會填入示範資料**(36 筆歷史、14 筆警報)。網頁在離線或第一次同步前,
   會把示範資料當真的顯示。
9. **部分 setter 沒有做等值比較**(`motorRunningSv/Pv`、清單屬性)。pack 文件 §6.1 要求
   值不變時不要發 signal。本輪沒改,因為這會影響桌面重送指令的行為,和安全有關。
10. **每次啟動都寫入一筆「設備啟動」警報**到 SQLite。
11. **`main` 和 `core` 分支已分岔**,各有 8 個 commit。本整合只在 `core` 上。
12. **歷史頁的離線橫幅會遮住內容**:網頁斷線時,紅色橫幅疊在「歷史資料」標題與「下載 CSV」
    按鈕上方(證據 `docs/evidence/wasm-v4-csv/40-*.png`)。主頁沒有被遮到重要內容。
13. **Modbus 斷線期間的指令會遺失,重連後也不補送**(依程式碼判讀,未在真實設備實測):

    | 連線 | 重連機制 |
    |---|---|
    | 5 台 ADAM(TCP) | 每台獨立計時器,斷線或連線失敗後固定每 3 秒重試(無退避);有讀寫請求時發現未連線也會立刻重試。單次請求逾時 1 秒、重試 2 次 |
    | MS300(COM2 RTU) | 無專用計時器,每 1 秒輪詢時順便重連 |
    | Modbus server(502) | 只在啟動時開一次;失敗只記 log,不再重試,程式照常執行 |

    - 未連線時的讀寫一律丟棄,只記 `Device is not connected; request was not sent.`,不排隊
      (`Modbus_Client.cpp` `ensureConnected`)。
    - 重連成功後 Core 只記一行「connected」(`manager.cpp` `deviceConnectionChanged`),
      **不會把畫面上的目前設定值寫回設備**。
    - 後果:斷線那幾秒內改的設定值(例如 M1)畫面有、設備沒有;若按下緊急停止時 ADAM-6256
      剛好斷線,關馬達的指令不會送出,重連後也不補送,畫面顯示急停 ON 但馬達可能仍在轉。
    - 寫入失敗只記在 log,畫面上看不出異常。

### 4.4 本輪追加修正:網頁版「下載 CSV」(w2-028)

- 修改:`saveHistoryCsv` 在網頁版(`#if defined(Q_OS_WASM)`)改用 Qt 為 WebAssembly 提供的
  `QFileDialog::saveFileContent`,由瀏覽器下載檔案;內容與桌面版相同(UTF-8 BOM + 同一份 CSV),
  建議檔名 `TaidaFlow_History_yyyyMMdd_HHmmss.csv`,畫面顯示「已開始下載 N 筆資料(檔名)」。
  桌面版程式碼原封不動(放在 `#else`)。改動只有新增:Proxy 20 行、QML 5 行。
- 離線時仍可匯出:這是不送指令的本機功能,資料已在網頁端。
- 驗證:桌面匯出、網頁線上下載、網頁離線下載、瀏覽器實際寫入的檔案,4 份 CSV 位元組完全相同,
  且與 8 筆測試資料逐格一致(18 欄、含 BOM 與中文)。**Mango 於 2026-09-24 在實際瀏覽器確認
  網頁下載可用。**
- 注意:Chrome/Edge 會跳出瀏覽器原生的「另存新檔」視窗;Firefox/Safari 直接下載。Qt 不回報
  使用者是否取消,所以訊息是「已開始下載」而非「已下載」。
- 測試資料:歷史資料由 `scripts/seed_history_sqlite.py` 寫進 Core 自己的 SQLite(在程式關閉時),
  Core 的讀取路徑是真的;Core 自行寫入歷史資料的路徑因沒有設備而未測。

### 4.5 pack 文件的落差(給 pack 維護方)

| 編號 | 內容 |
|---|---|
| G-1 | 文件暗示 HttpServer、Sql 也要移到桌面分支;實際上 wasm_singlethread 有這兩個,缺的是 SerialBus、SerialPort、Concurrent。WASM 的 CMakeCache 會出現 `Qt6Sql_DIR`,但那是 Qt 自己的 QML plugin 掃描造成的,並沒有連結,稽核時容易誤判 |
| G-2 | 沒說明當 Proxy 標頭屬於另一個 STATIC QML module 時,註冊應放在哪個 target。本案放在擁有標頭的 `Core` target,驗證可行 |
| G-3 | 每次重連失敗,`transportStateHandler` 會連續被呼叫兩次;從沒連上過也顯示「connection was lost」;訊息寫死英文。文件沒有列出訊息集合,也沒提在地化 |
| G-4 | §9 範例沒說明要先更新 ready 和 message,再發通知;逐一發出會讓畫面短暫看到不一致的組合 |
| G-5 | 沒提醒 replica 建構子若填有示範資料,網頁在第一次同步前會把它當真資料顯示 |
| G-6 | 沒提 `Q_INVOKABLE` 不會被轉送,會在網頁本機執行 |
| G-7 | 路徑範例不一致:§5.1、§7 用 `/mirror`,guide §10 用 `/` |
| G-8 | 只要求驗證 MANIFEST,沒提醒 `core.autocrlf=true` 會在 checkout 時破壞 hash;建議附 `.gitattributes` 範本 |

---

## 5. 維護方建議

依優先順序排列。

### 高(安全相關,上線前處理)

1. **決定網頁端能不能操作緊急停止,特別是「解除」**。目前網頁上的任何人都能解除急停。
   建議至少禁止網頁端解除急停;或加上確認步驟與權限控管。
2. **保留離線鎖定,之後新增的控制項也要比照**。以後加任何會送指令的按鈕、開關或對話框,
   都要綁定 `Td.transportReady`(可參考 `Main.qml` 的 `controlsEnabled`)。
3. **Modbus 重連後補送目前設定值**(§4.3-13)。收到設備「已連線」時,把該設備負責的
   輸出(DO、AO)依畫面上的目前值寫一次,讓畫面和設備一致。
4. **緊急停止要確認送達**(§4.3-13)。急停指令等設備回應成功,失敗就持續重送,並在畫面顯示
   「急停未送達」警示;同時確認現場有硬體急停迴路,軟體急停不能取代它。
5. **寫入失敗要讓操作員看得到**。目前只記 log,畫面上沒有任何異常提示。
6. **Modbus server 不要綁 0.0.0.0**。若不需要對外,改綁 127.0.0.1 或指定網卡。
   也要注意它會和本機其他 502 服務衝突;啟動失敗時至少要在畫面上提示,或定時重試。
7. ~~處理 `saveHistoryCsv` 在網頁版的行為~~ —— **已於本輪完成**(§4.4)。

### 中(正確性與維運)

8. **設備位址改成可設定**(ADAM IP、MS300 COM port,例如放進 `TaidaFlowSettings.ini`)。
   這樣可以接 `Adam60xxSimulator` 做開發測試,也降低開發機誤連真實設備的風險。
9. **決定歷史頁頁碼要不要共享**。若希望每個畫面各自翻頁,把頁碼改成各端自己保存,
   翻頁改用 request signal 向桌面查詢。
10. **修正 `data_schema.sql` 找不到**。部署時一起帶這個檔案,或改用 qrc 內嵌。
11. **補上 setter 等值比較**(pack 文件 §6.1),並確認不影響重送指令的語意。
12. **正式版移除 Proxy 建構子的示範資料**,或只在沒有 Core 時才填。
13. **重試加退避、降低 log 量**:ADAM 固定 3 秒、MS300 每秒重試,沒有設備時 log 量很大;
    改成逐步拉長間隔,相同錯誤合併記錄。
14. **調整離線橫幅位置**:改成把頁面內容往下推,而不是疊在上面,避免遮住歷史頁的標題與
    「下載 CSV」按鈕(§4.3-12)。

### 低(維護整潔)

15. **不要修改 `integration-pack/wasm-mirror/` 裡的檔案**。客製一律做在宿主專案;要升級時
    整包替換,再跑 `scripts/verify_pack.py` 驗證。1.0.0 的問題就是改了 pack 內部又改回來造成的。
16. **刪除 `integration-pack/wasm-mirror-1.0.0.zip`**。已經過時,容易混淆。
17. **`.gitignore` 不要再排除 `CMakePresets.json`**。網頁版建置需要它。
18. **`main` 和 `core` 的分岔要找時間合併**,否則網頁版整合只存在於 `core`。

---

## 附錄:如何執行

```bat
scripts\build-desktop.bat            :: 桌面版
scripts\build-wasm.bat wasm-release  :: 網頁版
```

1. 啟動桌面版:`powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1`
   (會先做安全探測,並以 `build\runtime-cwd` 為工作目錄)
2. 啟動網頁伺服器:`python scripts\serve_wasm.py`
3. 瀏覽器開 `http://127.0.0.1:8123/TaidaFlowApp.html`

詳細說明見 `README.md`。
