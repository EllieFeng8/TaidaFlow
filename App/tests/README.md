# TaidaFlow App unit tests (w2-061, w2-064)

## 目的
測試 `App/appconfig.{h,cpp}`(桌面版 config.json 讀取器,規格 `docs/taidaflow_config_spec.md` §1/§2)
與 `App/runtimeinfo.{h,cpp}`(網頁版讀 `/runtime.json` 取得同步 port,規格 §3),
以及 `App/applog.{h,cpp}`(w2-064:桌面版程式自己寫 log 檔,規格 §2 `log`),
和介面字型(w2-082:`App/main.cpp` 的 `applyUiFont()` 與 HOOK、`App/embeddedfonts.cpp`、`App/fonts/*.ttf`)。
這是獨立的小 CTest 專案,直接編譯 `App/` 裡的原始檔,不改動 Qt Design Studio 產生的主 CMake。

| 測試 | 內容 |
|---|---|
| `tst_appconfig` | 預設值 = 規格 §2、預設檔格式(UTF-8 無 BOM、2 格縮排、順序)、缺檔建立預設、`TAIDAFLOW_CONFIG` 指定、資料夾不可寫 → 記憶體預設、JSON 錯誤不覆寫且回報行列與對話框文字、缺鍵補預設、未知鍵忽略、型別/範圍錯誤用預設、相對/絕對 dataDir 與 `applyDataDir()`、下載 port 推導與 `TAIDAFLOW_DOWNLOAD_PORT` 覆寫、`--write-default-config`(成功/已存在不覆寫/不可寫/缺路徑);w2-064:`nginx.exe`(預設 `nginx\nginx.exe`,相對 config.json 資料夾解析、絕對路徑照用)、`log` 區段(預設值、缺鍵、型別/範圍錯誤、非物件群組、`log.dir` 相對 dataDir 解析) |
| `tst_runtimeinfo` | `/runtime.json` 解析(有效/無效/缺欄)、產生;非同步請求對 127.0.0.1 上真實 HTTP 回應端:200 有效、404、內容無效、逾時、連線被拒、URL 無效、context 先銷毀 |
| `tst_applog` | config.json 無法使用時寫到備援資料夾(A2:壞 JSON、路徑是資料夾、備援資料夾不可寫仍 exit 2 且不改 config.json)、檔名 `taidaflow-YYYY-MM-DD.log`(quiet:warning/critical/fatal)與 `-full.log`(全部)同時寫與層級過濾、每行格式(時間戳毫秒、層級、category)、UTF-8 無 BOM + CRLF、log 資料夾自動建立、同日重啟接續寫、換日(往後與系統時間往回調)、保留天數邊界(1/7/60,含今天)、只刪完全符合命名的檔(舊版啟動腳本 log、nginx log、子資料夾等不動)、啟動與換日清理、`enabled=false`、寫檔失敗(資料夾是檔案、唯讀檔、非法字元)不當機且 60 秒/換日重試、8 執行緒同時寫不交錯、真實 message handler(安裝前緩衝重放、category、轉送原 handler、多執行緒)、清理耗時、緩衝上限 |

全部使用 `QTemporaryDir` 內的真實檔案與真實 TCP 連線,沒有模擬物件。
`tst_uifont` / `tst_uifont_wasmpath`(w2-082)測的程式碼**不是複本**:configure 時 `CMakeLists.txt` 從 `App/main.cpp` 切出
`applyUiFont()` 整個函式,以及 `main()` 裡「(core only)」區塊(`loadEmbeddedCjkFont`、WebAssembly 才執行的 `installCjkFallbackChain`)
與 HOOK 區塊(`uiFontFamilyRequest` 那一行 + `applyUiFont(...)`),產生 `build\app-tests\uifont_gen\*.inc` 後編譯
(`main.cpp` 是 configure 相依檔,改了就重新產生;找不到標記時 configure 失敗)。`tst_uifont` 是桌面版編譯出的順序;
`tst_uifont_wasmpath` 把同一段文字的 `#if defined(Q_OS_WASM)` 打開(網頁版的順序:先 fallback 鏈再 `applyUiFont`),
在桌面的字型資料庫上執行(沒有開瀏覽器;網頁上 `Consolas` 的第一個替代是 DejaVu Sans Mono,這裡是桌面的等寬字型 Courier New)。
兩者檢查:`main.cpp` 只有一行 HOOK 且內容正確、呼叫順序(QApplication → 載入內嵌字型 → [WASM fallback 鏈] → HOOK → `applyUiFont`
→ QML engine);應用程式字型、`QFontInfo`、`QRawFont`(一般 400 / 粗體 700 都是內嵌檔)都是 `TaidaFlow Noto Sans TC`;
log 有 `[UiFont] interface font: TaidaFlow Noto Sans TC`;`Consolas` 替代清單(桌面只有內嵌字型、網頁順序內嵌字型在最後);
真實 UI 字串(中英文、數字、°、·、−、全形標點)的每個字形都來自內嵌字型;Consolas 數值裡的中文來自內嵌字型(桌面的數字仍是 Consolas);
兩個 TTF 都含可列印 ASCII、UI 符號、Latin-1 補充與 `charset.txt` 全部字元;`App.qml` 根物件是 `T.ApplicationWindow`、
其 `font.family` 綁定(從 `App.qml` 讀出,不是寫死)在 Universal style 下讓視窗、Label、Text、Button 與 Popup 裡的 Label
都是內嵌字型(`App.qml` 其餘內容與頁面不載入;全頁面逐元件的檢查見 main 的 w1-081)。預設 Windows 平台外掛,不顯示任何視窗。
`tst_applog` 唯一注入的是日期時間來源(可設定的時鐘),用來測換日;寫檔、刪檔、執行緒、Qt message handler 都是真的。

## 建置與執行(Windows,MSVC)
在 repo 資料夾(或任何資料夾;腳本以自己的位置找 repo)開 cmd:
```bat
App\tests\run-app-tests.bat          :: configure + build + CTest,輸出 build\app-tests(增量)
App\tests\run-app-tests.bat fresh    :: 先刪 build\app-tests 再從頭建
```
exit code 0 = 5 個測試全部通過(`100% tests passed, 0 tests failed out of 5`)。與 `Core\tests\run-core-tests.bat` 同一種做法。
工具不在預設位置時設 `TAIDAFLOW_QT_ROOT`、`TAIDAFLOW_QT_TOOLS`、`TAIDAFLOW_VCVARS64`(與 `scripts\build-desktop.bat` 相同,
`docs\BUILD.md` §2.6);`fresh` 之後的參數會交給 ctest(例如 `-R tst_applog`)。

等同於(在 repo 資料夾的 cmd;VS / Qt 路徑依實際位置改):
```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
C:\Qt\Tools\CMake_64\bin\cmake.exe -S App\tests -B build\app-tests -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\app-tests
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir build\app-tests --output-on-failure
```
CTest 會自動把 Qt 的 `bin` 加到 PATH;每個測試的 QTest 結果另存為 `build\app-tests\tst_*.result.txt`。
用 CLion 時可直接 File → Open `App\tests` 資料夾(Toolchain 選 Visual Studio、CMake options
`-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64`,`docs\BUILD.md` §6A.6)。

## 應用程式端的相關開關
- `TAIDAFLOW_CONFIG=<完整路徑>`:使用指定的 config.json(開發/測試用)。
- `TaidaFlowApp.exe --write-default-config <路徑>`:只寫出預設 config.json 後結束
  (0 = 成功,1 = 無法寫入,3 = 檔案已存在(不覆寫),4 = 缺路徑)。
- config.json 無法解析時程式顯示錯誤對話框並以 exit code 2 結束;
  `TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS=<毫秒>` 只供無人值守的檢查用,讓對話框在時間到後自動關閉。
- log 檔(w2-064,僅桌面版):`<dataDir>\<log.dir>`(預設 `C:\TaidaFlowData\logs`)內每天
  `taidaflow-YYYY-MM-DD.log`(quiet,保留 `log.quiet.keepDays`=60 天)與 `taidaflow-YYYY-MM-DD-full.log`
  (full,保留 `log.full.keepDays`=7 天),同時仍輸出到 stderr/除錯器。實跑驗證(會啟動程式,先跑安全探測,SAFE 才啟動):
  `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-desktop-startup.ps1`(開發機,log 寫到開發設定的
  `build\runtime-cwd\logs`);打包資料夾的當天 quiet / full log 由 `scripts\verify-release-package.ps1` 檢查(README「測試 / 驗證」5g)。
- config.json 無法使用時(w2-064 A2):log 改寫到備援資料夾 `<config.json 所在資料夾>\logs`(同樣檔名、預設保留天數與清理),
  內容含緩衝的訊息與失敗原因(路徑、行列、錯誤),錯誤對話框與 stderr 註明 log 檔位置,仍以 exit code 2 結束、不改 config.json;
  備援資料夾不可寫時只少了 log 檔,其餘處理不變。測試:`tst_applog` 的 `configFailure*`;實跑(壞 JSON → 啟動腳本與
  `TaidaFlowApp.exe` 都 exit 2、備援 log 在打包資料夾 `logs\`、對話框以 `TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS` 自動關閉,無 UI 操作):
  `scripts\verify-release-package.ps1` 的 D 項(README「測試 / 驗證」5g)。
