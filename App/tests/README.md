# TaidaFlow App unit tests (w2-061)

## 目的
測試 `App/appconfig.{h,cpp}`(桌面版 config.json 讀取器,規格 `docs/taidaflow_config_spec.md` §1/§2)
與 `App/runtimeinfo.{h,cpp}`(網頁版讀 `/runtime.json` 取得同步 port,規格 §3)。
這是獨立的小 CTest 專案,直接編譯 `App/` 裡的原始檔,不改動 Qt Design Studio 產生的主 CMake。

| 測試 | 內容 |
|---|---|
| `tst_appconfig` | 預設值 = 規格 §2、預設檔格式(UTF-8 無 BOM、2 格縮排、順序)、缺檔建立預設、`TAIDAFLOW_CONFIG` 指定、資料夾不可寫 → 記憶體預設、JSON 錯誤不覆寫且回報行列與對話框文字、缺鍵補預設、未知鍵忽略、型別/範圍錯誤用預設、相對/絕對 dataDir 與 `applyDataDir()`、下載 port 推導與 `TAIDAFLOW_DOWNLOAD_PORT` 覆寫、`--write-default-config`(成功/已存在不覆寫/不可寫/缺路徑) |
| `tst_runtimeinfo` | `/runtime.json` 解析(有效/無效/缺欄)、產生;非同步請求對 127.0.0.1 上真實 HTTP 回應端:200 有效、404、內容無效、逾時、連線被拒、URL 無效、context 先銷毀 |

全部使用 `QTemporaryDir` 內的真實檔案與真實 TCP 連線,沒有模擬物件。

## 建置與執行(Windows,MSVC)
```bat
D:\repo\codex\qmlTester\taidaflow-main\build\w2-061-tools\build-tests.bat
```
等同於:
```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
C:\Qt\Tools\CMake_64\bin\cmake.exe -S App\tests -B build\w2-061-tests -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\w2-061-tests
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir build\w2-061-tests --output-on-failure
```
CTest 會自動把 Qt 的 `bin` 加到 PATH;每個測試的 QTest 結果另存為 `build\w2-061-tests\tst_*.result.txt`。

## 應用程式端的相關開關
- `TAIDAFLOW_CONFIG=<完整路徑>`:使用指定的 config.json(開發/測試用)。
- `TaidaFlowApp.exe --write-default-config <路徑>`:只寫出預設 config.json 後結束
  (0 = 成功,1 = 無法寫入,3 = 檔案已存在(不覆寫),4 = 缺路徑)。
- config.json 無法解析時程式顯示錯誤對話框並以 exit code 2 結束;
  `TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS=<毫秒>` 只供無人值守的檢查用,讓對話框在時間到後自動關閉。
