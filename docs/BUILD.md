# TaidaFlow 編譯說明(core 分支,給開發人員)

> 對象:要在**自己的電腦**上從零開始 clone、安裝工具、編譯 TaidaFlow 桌面版與網頁版的開發人員。
> 部署與啟動(正式機 / 測試機、給操作人員)另見 [DEPLOY_AND_STARTUP.md](DEPLOY_AND_STARTUP.md)。
> 本文件只適用 **`core` 分支**;`main` 分支(UI)沒有本文件提到的 `scripts\`、`CMakePresets.json`,建置方式不同(§8)。
> 文件中的指令都是在參考開發機上實際執行過的(exit code 證據在 `docs/evidence/w2-063/`);
> **哪些部分沒辦法在全新電腦上驗證**,見 §11。

目錄:
[0 快速開始](#0-快速開始) ·
[1 取得原始碼](#1-取得原始碼) ·
[2 需要的軟體](#2-需要的軟體) ·
[3 專案與 CMake 結構](#3-專案結構與-cmake-結構) ·
[4 桌面版編譯](#4-桌面版編譯) ·
[5 網頁版編譯](#5-網頁版編譯webassembly) ·
[6 Qt Creator](#6-用-qt-creator-開專案) ·
[7 編譯後必跑的檢查](#7-編譯後必跑的檢查) ·
[8 分支分工與合併](#8-分支分工main--ui--core--後端與-main-合併進-core-之後) ·
[9 常見問題](#9-常見問題) ·
[10 config.json](#10-configjson設定檔) ·
[11 驗證紀錄](#11-驗證紀錄與沒有驗證到的部分)

---

## 0. 快速開始

工具都照 §2 的**預設位置**裝好之後,在 repo 資料夾開 cmd:

```bat
scripts\build-desktop.bat            :: 桌面版 -> build\desktop\TaidaFlowApp.exe
scripts\build-wasm.bat               :: 網頁版 -> build\wasm-release\TaidaFlowApp.html/.js/.wasm
```

兩支都是 exit code 0 = 成功。工具裝在別的位置:先設環境變數(§2.6)再跑同樣的指令。
第一次在新電腦上做,請從 §1 依序往下,最後用 §2.7 的檢查清單確認。

---

## 1. 取得原始碼

### 1.1 clone 與切到 core 分支

需要 Git for Windows(<https://git-scm.com/download/win>,安裝選項用預設即可)。

```bat
mkdir C:\src
cd /d C:\src
git clone -b core https://github.com/EllieFeng8/TaidaFlow.git TaidaFlow
cd TaidaFlow
git status --short --branch
```

- 公開 repo,不需要登入(2026-09-28 以不帶認證的 `git ls-remote` 確認 `core`、`main` 兩個分支都在)。
- `git status --short --branch` 第一行應為 `## core...origin/core`。
- 已經 clone 了預設分支(`main`)時,用 **`git switch core`** 切換。**不要用 `git checkout core`**:Windows 不分大小寫,
  它和 repo 裡的 `Core\` 資料夾撞名,會出現 `fatal: 'core' could be both a local file and a tracking branch.`
  (實測,§11)。

### 1.2 兩個分支

| 分支 | 內容 | 誰改 |
|---|---|---|
| `main` | UI:Qt Design Studio 產生的畫面(`TaidaFlow/`、`TaidaFlowContent/`、`Dependencies/`)與 `Core/TaidaFlowProxy.h`(QML 看到的 `Td` 介面) | UI 開發 |
| `core` | `main` 的全部 + 真正的後端 `Core/`(Modbus、MS300、SQLite、REST、HTTP 服務、歷史匯出)、網頁版整合(`integration-pack/`、`App/wasm/`、內嵌字型)、`scripts/`、`deploy/`、`docs/`、`CMakePresets.json` | 後端開發 |

UI 修改先進 `main`,再把 `main` 合併進 `core`(合併後要做的事見 §8)。要編譯能連設備的完整程式,一律用 `core`。
切換分支一律用 `git switch <分支>`(`git checkout core` 在 Windows 會和 `Core\` 資料夾撞名,§1.1)。

### 1.3 repo 放在哪裡

- **路徑要短**:建議 repo 資料夾本身不超過 40 個字元,例如 `C:\src\TaidaFlow`、`D:\work\TaidaFlow`。
  實測 `build\desktop` 與 `build\wasm-release` 裡最深的檔案,相對 repo 資料夾的路徑已有 202 / 206 個字元;
  Windows 傳統路徑上限 260 字元,repo 路徑太長時編譯器或工具可能找不到檔案。
- **不要有空白、中文或特殊字元**(`( ) $ " ' { } ; # &` 等)。nginx 腳本會拒絕含 `$ " ' { } ; #` 的路徑,
  批次檔對空白與括號也容易出錯(本專案的 .bat 已加引號,但只在不含空白的路徑實測過,§11)。
- 不要放在 OneDrive / Dropbox 等同步資料夾(建置時大量寫檔,同步程式可能鎖住檔案)。
- 所有建置輸出都在 repo 裡的 `build\`(已被 `.gitignore` 排除),不會寫到 repo 以外的地方。

### 1.4 repo 內要知道的兩件事

- `CMakePresets.json` **有在版本控制裡**(clone 就會拿到),但 `.gitignore` 也列了 `/CMakePresets.json`:
  修改後 `git add` 會被忽略,要用 `git add -f CMakePresets.json`。自己的路徑請不要改它,改用
  `CMakeUserPresets.json`(已被 `.gitignore` 排除,§2.6)。
- `integration-pack/wasm-mirror/` 是由維護方提供的網頁同步套件(pack 1.0.1),**唯讀、整包複製**。
  `.gitattributes` 設了 `-text`,不論 `core.autocrlf` 怎麼設都不會改換行,`MANIFEST.sha256` 才能驗證通過
  (`powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-pack.ps1` 應印 24/24 並 exit 0,§7)。

---

## 2. 需要的軟體

### 2.1 總表

| 軟體 | 版本 | 預設安裝位置(本專案腳本的預設值) | 用途 |
|---|---|---|---|
| Qt | **6.8.3**(只能用這版) | `C:\Qt\6.8.3` | 桌面 kit `msvc2022_64`、網頁 kit `wasm_singlethread` |
| CMake | 3.30.5(Qt 安裝程式附的;最低 3.25,因 `CMakePresets.json` 是 version 6) | `C:\Qt\Tools\CMake_64` | 產生建置檔 |
| Ninja | 1.12.1(Qt 安裝程式附的) | `C:\Qt\Tools\Ninja` | 實際編譯 |
| Visual Studio | 2022(17.x)或更新,含「使用 C++ 的桌面開發」 | `C:\Program Files\Microsoft Visual Studio\<版本>\<版別>` | MSVC 編譯器(桌面版) |
| emsdk | **3.1.56**(Qt 6.8 對應版,不能換) | `C:\tools\emsdk` | Emscripten 編譯器(網頁版) |
| nginx for Windows | 1.30.x(參考機 1.30.5)| `C:\tools\nginx\nginx-<版本>` | 選用:桌面版建置時複製到 `build\desktop\nginx`(§4.5)、打包時隨包附上;沒有也能編譯 |
| Python + fonttools | 3.x + `fonttools` | 任意 | **選用**:只有「重新產生網頁版內嵌字型」需要(§2.5);一般桌面版 / 網頁版編譯、所有檢查腳本與部署都**不需要** |
| Git | 任意 | 任意 | clone、安裝 emsdk |

參考開發機(本文件指令的驗證環境):Windows 11 Pro 10.0.26200、Qt 6.8.3、CMake 3.30.5、Ninja 1.12.1、
Visual Studio Community **18**(18.9.3,MSVC `cl` 19.51)、emsdk 3.1.56、nginx 1.30.5、Git 2.55。
本專案的檢查、執行、打包腳本都是 PowerShell(`scripts\*.ps1`)或批次檔(w2-062 起沒有其他 Python 腳本)。

### 2.2 Qt 6.8.3(Qt Online Installer)

1. 從 <https://www.qt.io/download-qt-installer> 下載 Qt Online Installer,用 Qt 帳號登入(沒有就免費註冊)。
2. 安裝資料夾用預設 `C:\Qt`(換位置可以,之後照 §2.6 設 `TAIDAFLOW_QT_ROOT` / `TAIDAFLOW_QT_TOOLS`)。
3. 選「自訂安裝」(Custom installation)。6.8.3 不是 6.8 的最新修正版時,清單預設看不到:勾右側的
   **Archive** 再按 **Filter**,才會出現「Qt 6.8.3」。
4. 在 **Qt → Qt 6.8.3** 底下勾:

   | 勾選項目(安裝程式顯示的名稱) | 元件 ID(參考機 `C:\Qt\components.xml`) | 必要性 |
   |---|---|---|
   | MSVC 2022 64-bit | `qt.qt6.683.win64_msvc2022_64` | 必要(桌面版;也是網頁版的 host 工具) |
   | WebAssembly (single-threaded) | `qt.qt6.683.wasm_singlethread` | 必要(網頁版) |
   | Additional Libraries → Qt 5 Compatibility Module | `qt.qt6.683.addons.qt5compat` | 必要(QML 用 `Qt5Compat.GraphicalEffects`,46 處) |
   | Additional Libraries → Qt HTTP Server | `qt.qt6.683.addons.qthttpserver` | 必要(桌面:REST、8124 網頁與下載) |
   | Additional Libraries → Qt Quick Timeline | `qt.qt6.683.addons.qtquicktimeline` | 必要 |
   | Additional Libraries → Qt Serial Bus | `qt.qt6.683.addons.qtserialbus` | 必要(桌面:Modbus) |
   | Additional Libraries → Qt Serial Port | `qt.qt6.683.addons.qtserialport` | 必要(桌面:MS300 COM2) |
   | Additional Libraries → Qt Shader Tools | `qt.qt6.683.addons.qtshadertools` | 必要 |
   | Additional Libraries → Qt WebSockets | `qt.qt6.683.addons.qtwebsockets` | 必要(桌面 ↔ 網頁同步) |
   | WebAssembly (multi-threaded) | `qt.qt6.683.wasm_multithread` | 只有 `main` 分支的網頁版要(core 不用) |

   Additional Libraries 的模組會自動裝到上面勾的每個 kit(桌面與 WebAssembly 都有);Serial Bus / Serial Port
   沒有 WebAssembly 版,這是正常的(網頁版不編後端)。Qt Core / Gui / Qml / Quick / Quick Controls / Network /
   Sql / Concurrent / Svg / Shapes 已包含在 kit 本身,不用另外勾。
5. 在 **Qt → Build Tools**(有些版本叫 Developer and Designer Tools)勾 **CMake 3.30.x** 與 **Ninja 1.12.1**。
   Qt Creator 可選(§6)。
6. 同意授權後安裝。

驗證(開 cmd;路徑依實際安裝位置改):

```bat
C:\Qt\6.8.3\msvc2022_64\bin\qtpaths.exe --qt-version
C:\Qt\Tools\CMake_64\bin\cmake.exe --version
C:\Qt\Tools\Ninja\ninja.exe --version
dir /b C:\Qt\6.8.3\msvc2022_64\lib\cmake | findstr /x "Qt6Core5Compat Qt6HttpServer Qt6QuickTimeline Qt6SerialBus Qt6SerialPort Qt6ShaderTools Qt6WebSockets"
dir /b C:\Qt\6.8.3\wasm_singlethread\lib\cmake | findstr /x "Qt6Core5Compat Qt6QuickTimeline Qt6ShaderTools Qt6WebSockets"
findstr /c:"QT_EMCC_VERSION" C:\Qt\6.8.3\wasm_singlethread\include\QtCore\qconfig.h
```

預期:`6.8.3`;`cmake version 3.30.5`;`1.12.1`;第一個 `dir` 列出 7 行、第二個列出 4 行(少一行就是那個模組沒勾,
回安裝程式的「Qt Maintenance Tool → 新增或移除元件」補裝);最後一行 `#define QT_EMCC_VERSION "3.1.56"`
(這就是 emsdk 必須用 3.1.56 的原因)。

> 也可以用命令列安裝(`qt-online-installer-windows-x64-<版本>.exe --root C:\Qt --accept-licenses
> --default-answer --confirm-command install <上表元件 ID> qt.tools.cmake qt.tools.ninja`),
> 但**本專案沒有實測過命令列安裝**,元件 ID 取自參考機已安裝的清單。

### 2.3 Visual Studio(MSVC 編譯器)

1. 安裝 Visual Studio 2022 或更新版(Community 即可;只要編譯器也可以裝「Build Tools for Visual Studio」)。
2. 工作負載勾 **「使用 C++ 的桌面開發」(Desktop development with C++)**。右側預設會帶 MSVC x64/x86 建置工具
   與 Windows 11 SDK,保持勾選。
3. 找出 `vcvars64.bat`(之後要用):

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat
```

   常見位置:

   | 版本 | vcvars64.bat |
   |---|---|
   | VS 18 Community(參考機,腳本預設) | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat` |
   | VS 2022 Community | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat` |
   | VS 2022 Professional / Enterprise | 同上,`Community` 換成 `Professional` / `Enterprise` |
   | Build Tools 2022 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |

   不是 VS 18 Community 時,照 §2.6 設 `TAIDAFLOW_VCVARS64`。

驗證:

```bat
call "<上面找到的 vcvars64.bat>"
cl
```

`cl` 印出 `Microsoft (R) C/C++ Optimizing Compiler Version 19.xx ... for x64` 即可(參考機 19.51)。
Qt 6.8.3 的 `msvc2022_64` 是用 VS 2022 的 MSVC 編的;較新的 VS(參考機 VS 18)與它二進位相容,參考機即此組合。
**VS 2022 本身沒有在本專案實測過**(§11)。

### 2.4 emsdk 3.1.56(Emscripten,網頁版用)

emsdk 是 Emscripten(把 C++ 編成 WebAssembly 的編譯器)的安裝管理工具。本專案的網頁版**只能**用 Emscripten **3.1.56**。

#### 2.4.1 為什麼一定是 3.1.56

- Qt 官方文件 *Qt for WebAssembly*(<https://doc.qt.io/qt-6.8/wasm.html#installing-emscripten>)列出每個 Qt 小版本對應的
  Emscripten 版本,**Qt 6.8 = 3.1.56**;同一個小版本的修正版(6.8.x)不會換版本。Qt 的預編譯 WebAssembly kit 就是用這一版
  編的,文件說明應用程式必須用同一版,因為 Emscripten 不保證不同版本之間的 ABI 相容。
- 本機可以直接確認:`findstr /c:"QT_EMCC_VERSION" C:\Qt\6.8.3\wasm_singlethread\include\QtCore\qconfig.h` →
  `#define QT_EMCC_VERSION "3.1.56"`。版本不同時 CMake configure 會印 `Qt Wasm built with Emscripten version: 3.1.56` /
  `You are using Emscripten version: ...` / `This may not work correctly`,之後可能連結失敗或執行時出錯。

#### 2.4.2 事前需要

- **Git for Windows**(<https://git-scm.com/download/win>):emsdk 本身是用 git clone 取得的。
- **emsdk 安裝程式本身的需求**(不是本專案的腳本):emsdk 裝好後用它**自帶**的直譯器與 Node,但**第一次** `emsdk install`
  時還沒有自帶的,`emsdk.bat` 會執行 PATH 上的 `python`(emsdk 官方的安裝需求,實測)。參考機上 cmd 的 `python` 是
  Microsoft Store 的別名,第一次安裝就失敗(`The system cannot find the file ...\WindowsApps\python.exe.`)。解法:在同一個 cmd
  視窗先執行 `for /f "delims=" %p in ('py -3 -c "import sys,os;print(os.path.dirname(sys.executable))"') do set "PATH=%p;%PATH%"`
  把真正的直譯器放到 PATH 最前面(寫進 .bat 時 `%p` 要寫成 `%%p`);裝好之後的網頁版編譯只用 emsdk 自帶的。
- 約 **1.5 GB** 磁碟空間(下載 + 解壓);可以連到 `github.com` 與 `storage.googleapis.com`(下載來源,實測)。
- 安裝資料夾:**不要有空白或中文**、**要可寫入**(`emsdk_env.bat` 每次執行都會在裡面寫暫存檔,Emscripten 也把編譯快取放在裡面),
  所以不要放 `C:\Program Files`。本專案預設 `C:\tools\emsdk`;放別處時設 `TAIDAFLOW_EMSDK`(§2.6)。

#### 2.4.3 安裝步驟(開一個新的 cmd,逐行輸入)

```bat
mkdir C:\tools
git clone https://github.com/emscripten-core/emsdk.git C:\tools\emsdk
cd /d C:\tools\emsdk
git checkout 3.1.56
.\emsdk install 3.1.56
.\emsdk activate 3.1.56
call .\emsdk_env.bat
emcc --version
```

逐行說明:

| 指令 | 做什麼 |
|---|---|
| `git clone ... C:\tools\emsdk` | 取得 emsdk。放別的資料夾(例如 `D:\sdk\emsdk`)也可以,之後設 `set TAIDAFLOW_EMSDK=D:\sdk\emsdk`(或 `setx` 永久設定),以下指令的路徑跟著換。 |
| `git checkout 3.1.56` | **建議**。讓 emsdk 本身停在 3.1.56 那一版,它附帶的 Node / Python 就會與 `CMakePresets.json` 寫的一樣(`node\16.20.0_64bit`、`python\3.9.2-nuget_64bit`,另有 `java\8.152_64bit`;參考機的 `C:\tools\emsdk` 就是 tag `3.1.56`、commit `e10826f`,2024-03-14)。不 checkout 也能裝 3.1.56,但 2026-09 的 emsdk 帶的是 Node 24.19.0、Python 3.13.3(實測),preset 的路徑對不上:`scripts\build-wasm.bat` 會自動改用不經 preset 的做法(實測可編),直接用 `cmake --preset` 則要自己寫 `CMakeUserPresets.json`(§2.6)。 |
| `.\emsdk install 3.1.56` | 下載並解壓 3.1.56 的編譯器(`wasm-binaries.zip` 約 461 MB)與附帶的 Node、Python(各約 30–40 MB)。參考機網路約 1 分鐘。前面的 `.\` 讓 cmd 一定用目前資料夾的 `emsdk.bat`。 |
| `.\emsdk activate 3.1.56` | 把 3.1.56 設成這個 emsdk 資料夾「使用中」的版本(寫入資料夾裡的 `.emscripten` 設定檔),並只對**目前這個 cmd 視窗**設定環境變數。**不要加 `--permanent`**(見下)。 |
| `call .\emsdk_env.bat` | 在目前視窗設定 `EMSDK`、`EMSDK_NODE`、`EMSDK_PYTHON` 與 PATH。**之後每開一個新的 cmd 要手動編網頁版之前都要先跑一次**:`call C:\tools\emsdk\emsdk_env.bat`。`scripts\build-wasm.bat` 會自己跑,不用手動。 |
| `emcc --version` | 驗證(下一小節)。 |

**要不要 `--permanent`**:

- 不加(本專案的做法):環境變數只在執行過 `emsdk_env.bat` 的視窗有效;其他程式、其他專案完全不受影響。
  代價是每個新視窗都要先 `call ...\emsdk_env.bat`(`build-wasm.bat` 已包含)。實測 `activate` 不加 `--permanent`
  時,使用者的永久環境變數(`HKCU\Environment` 的 `EMSDK`)前後都不存在。
- 加 `--permanent`:emsdk 把 `EMSDK`、`EMSDK_NODE`、`EMSDK_PYTHON` 與 emsdk 的 PATH 寫進使用者的永久環境變數,每個新視窗都直接有 `emcc`。
  代價:emsdk 的 Node / Python 會排到每個程式的 PATH 裡,可能蓋掉你自己的 Node / Python;電腦上有其他 Emscripten 版本或
  其他專案要用不同版本時容易混用;移除時要手動清環境變數。本專案**不需要**,不建議。

#### 2.4.4 驗證

```bat
call C:\tools\emsdk\emsdk_env.bat
emcc --version
type C:\tools\emsdk\upstream\emscripten\emscripten-version.txt
cd /d C:\tools\emsdk
.\emsdk list
```

預期(參考機與全新安裝的實測輸出):

- `emcc --version` 第一行:`emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) 3.1.56 (cf90417346b78455089e64eb909d71d091ecc055)`
- `emscripten-version.txt`(位置:`<emsdk>\upstream\emscripten\emscripten-version.txt`):`"3.1.56"`
- `emsdk list`:有一行 `3.1.56    INSTALLED`,使用中的工具前面有 `*`(例如 `(*)    node-16.20.0-64bit    INSTALLED`)。
- `dir /b C:\tools\emsdk\node` 與 `dir /b C:\tools\emsdk\python`:checkout 3.1.56 時為 `16.20.0_64bit` 與 `3.9.2-nuget_64bit`。

#### 2.4.5 常見錯誤

| 症狀 | 原因與解法 |
|---|---|
| `emcc --version` 不是 3.1.56 | 使用中的是別的版本:在 emsdk 資料夾 `.\emsdk activate 3.1.56`(沒裝過先 `install`),**開新視窗**再 `call ...\emsdk_env.bat`。仍不對時 `where emcc` 看第一個是不是別的 emsdk(例如以前用 `--permanent` 裝的):移除那個永久設定(§2.4.6)。 |
| `'emcc' is not recognized ...` | 這個視窗沒跑 `emsdk_env.bat`(每個新視窗都要),或 emsdk 還沒 `install` / `activate`。 |
| `'emsdk' is not recognized ...` | 不在 emsdk 資料夾,或 cmd 不搜尋目前資料夾(有設 `NoDefaultCurrentDirectoryInExePath` 時,本輪的自動化環境就是如此):用 `.\emsdk` 或完整路徑 `C:\tools\emsdk\emsdk.bat`。 |
| 第一次 `emsdk install` 出現 `The system cannot find the file ...\WindowsApps\python.exe` 或打開 Microsoft Store | PATH 上沒有能用的 Python(§2.4.2 的解法)。 |
| 下載很久沒動、`URLError`、`SSL`、`timed out`、`Connection refused` | 公司 proxy / 防火牆擋 `storage.googleapis.com` 或 `github.com`。有 proxy 時在同一視窗先 `set HTTPS_PROXY=http://<proxy主機>:<port>`(emsdk 用 Python 下載,會讀這個變數)、`git config --global http.proxy http://<proxy主機>:<port>`(git clone 用);或請 IT 放行這兩個網域。下載中斷後重跑 `.\emsdk install 3.1.56`;仍失敗就刪掉 `<emsdk>\downloads\` 裡不完整的 zip 再重跑。**本專案沒有在 proxy 環境實測。** |
| 路徑有空白或中文時各種奇怪錯誤 | 換到 `C:\tools\emsdk` 這種路徑重新 clone / install(含空白的路徑本專案沒有實測,不支援)。 |
| `emsdk_env.bat` 或編譯時寫檔失敗 / 權限錯誤 | emsdk 資料夾不可寫入(例如在 `C:\Program Files`),搬到 `C:\tools\emsdk`。 |
| 編譯時出現 `(Emscripten: config changed, clearing cache)`,接著 `ValueError: path is on mount 'D:', start on mount 'C:'` | 同一個 emsdk 被用**另一個路徑**使用(例如 junction / 符號連結 / 網路磁碟代號指到同一個資料夾)。Emscripten 的快取在 emsdk 資料夾裡,路徑一變就清空重建,跨磁碟時重建失敗(本輪實測踩到)。一律用 emsdk 真正的路徑;之後用原路徑再編一次,快取會自動重建(參考機即如此恢復)。 |

#### 2.4.6 移除與重新安裝

- 沒用過 `--permanent`:關掉所有用到它的 cmd 視窗,直接刪資料夾 `rmdir /s /q C:\tools\emsdk` 即可,系統其他地方沒有殘留。
- 用過 `--permanent`:另外到「系統內容 → 環境變數 → 使用者變數」刪除 `EMSDK`、`EMSDK_NODE`、`EMSDK_PYTHON`(以及有的話
  `EM_CONFIG`、`JAVA_HOME`),並從使用者 `Path` 移除所有 `...\emsdk...` 的項目,再開新視窗。
- 重新安裝:刪除後照 §2.4.3 從 `git clone` 重做。只是 Emscripten 快取壞掉時不必重裝:刪除 `<emsdk>\upstream\emscripten\cache`
  資料夾,下次編譯會自動重建。
- 本專案的建置輸出不含 emsdk 的東西;換 / 重裝 emsdk 後網頁版要 `scripts\build-wasm.bat wasm-release fresh`。

### 2.5 網頁版內嵌字型(字型子集)

網頁版(WebAssembly)沒有系統中文字型,所以把 Noto Sans TC 的**子集**(只含程式用到的字)編進 exe / wasm:
`App/fonts/TaidaFlowNotoSansTC-Regular.ttf`、`App/fonts/TaidaFlowNotoSansTC-Bold.ttf` 與字元清單 `App/fonts/charset.txt`。
這三個檔**已經在 git 裡**,建置時直接使用;CMake 不會產生它們,也不會呼叫任何外部程式。

**(A)使用 repo 內現成的字型(一般情況)**:什麼都不用做。clone 下來就能編桌面版與網頁版,不需要安裝 Python。

**(B)自己重新產生**(只有 UI / C++ 加了新的中文字、網頁出現方框時):這是整個專案**唯一**用到 Python 的地方
(`scripts\make_font_subset.py`,用 fonttools 子集化字型)。

1. 需要:
   - Python 3(<https://www.python.org/downloads/windows/>,安裝時勾「Add python.exe to PATH」;或用 Python 附的 `py -3` 啟動器)
     與 fonttools:`py -3 -m pip install fonttools`(或 `python -m pip install fonttools`)。
   - 來源字型:Windows 11 內附的 `C:\Windows\Fonts\NotoSansTC-VF.ttf`(預設);沒有時從
     <https://fonts.google.com/noto/specimen/Noto+Sans+TC> 下載 `NotoSansTC[wght].ttf`,以 `--source <檔案>` 指定。
2. 先檢查(不改任何檔):
   ```bat
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make-font-subset.ps1 --check
   ```
   exit 0 = 現有字型涵蓋所有字;exit 1 = 有新字(或缺字),要重新產生。`make-font-subset.ps1` 會先找 `py -3`,再找 PATH 上的
   `python`(跳過 Windows 市集的 `python.exe` 別名),以 `-B` 執行(不留 `__pycache__`);找不到可用的 Python 3 時清楚說明並 exit 9。
3. 重新產生:
   ```bat
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make-font-subset.ps1
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make-font-subset.ps1 --source "<下載的資料夾>\NotoSansTC[wght].ttf"
   ```
4. 重新編網頁版(`scripts\build-wasm.bat`)與桌面版,確認 `--check` 為 exit 0。
5. 把 `App/fonts/` 的**兩個 TTF 與 `charset.txt`**,和造成新字的程式修改放在**同一個 `core` commit** 一起提交。

註:`python` 在 cmd 可能被 Windows 的「應用程式執行別名」攔截(打開 Microsoft Store 或 exit 9059);`make-font-subset.ps1`
已跳過它。要自己在 cmd 打指令時改用 `py -3`,或到「設定 → 應用程式 → 進階應用程式設定 → 應用程式執行別名」關掉 `python.exe`。

### 2.6 工具路徑:裝在不同位置時怎麼改

**寫死預設路徑的地方**(只列建置相關;換位置時要處理):

| 檔案 | 行 | 寫死的路徑 | 換位置的做法 |
|---|---|---|---|
| `scripts\build-desktop.bat` | 18–20 | `C:\Qt\6.8.3`、`C:\Qt\Tools`、VS 18 `vcvars64.bat` | 設環境變數(下表),**不用改檔** |
| `scripts\build-wasm.bat` | 23–25、31–32 | `C:\Qt\6.8.3`、`C:\Qt\Tools`、`C:\tools\emsdk`(+ emsdk 內的 Python 3.9.2-nuget / Node 16.20.0) | 設環境變數,**不用改檔** |
| `CMakePresets.json` | 14、17 | desktop:Qt toolchain、Ninja | 建 `CMakeUserPresets.json`(下方範本),不要改這個檔 |
| `CMakePresets.json` | 32–48 | wasm:Qt wasm toolchain、Ninja、Emscripten、`QT_HOST_PATH`、node、emsdk 的 `EMSDK`/`EM_CONFIG`/`EMSCRIPTEN`/`EMSDK_PYTHON`/`EMSDK_NODE`/`PATH` | 同上 |
| `scripts\check-wasm-backend.ps1`、`scripts\check-version-shadow.ps1` | — | Ninja:`TAIDAFLOW_QT_TOOLS\Ninja\ninja.exe`,預設 `C:\Qt\Tools\Ninja\ninja.exe` | 設 `TAIDAFLOW_QT_TOOLS`,或給 `-Ninja <ninja.exe>` |
| `scripts\run-apphttpserver-tests.bat` | 9、10、12–14 | VS 18 `vcvars64.bat`、`C:\Qt\6.8.3\msvc2022_64`、`C:\Qt\Tools` | 改這幾行 |
| `scripts\run-pack-tests.bat` | 8、9、14、15、18、19 | 同上 | 改這幾行 |
| `docs\evidence\w2-049\tools\run-w2041-qtest.bat`、`docs\evidence\w2-052\tools\run-qtest.bat`、`docs\evidence\w2-062\tools\make-bench-db.bat` | vcvars、PATH、cmake 那幾行 | 同上 | 改那幾行 |
| `scripts\make_font_subset.py`(選用,§2.5) | 41 | `C:/Windows/Fonts/NotoSansTC-VF.ttf` | 用 `--source <字型>`,不用改檔 |
| `scripts\run-desktop.ps1`、`verify-release-package.ps1`、`taidaflow-config.ps1` | | 執行時把 Qt 的 bin 加到 PATH:`TAIDAFLOW_QT_ROOT\msvc2022_64\bin`,預設 `C:\Qt\6.8.3\msvc2022_64\bin` | 設 `TAIDAFLOW_QT_ROOT`(執行用,不影響編譯) |
| `scripts\run-simulator.ps1` | 51 | `C:\Qt\6.8.3\msvc2022_64\bin` 加到 PATH | 改那一行(執行用) |
| `scripts\package-release.ps1` | | `-QtDir` 預設 `C:\Qt\6.8.3\msvc2022_64`、ninja `C:\Qt\Tools\Ninja`、要附上的 nginx `-NginxDir`(預設 `C:\tools\nginx\nginx-<最新版本>`) | `-QtDir`、`-NginxDir` 參數;ninja 那行要改 |
| 根 `CMakeLists.txt`(桌面版的 nginx 資料夾,§4.5) | | nginx:快取變數 `TAIDAFLOW_NGINX_DIR`,預設 `C:\tools\nginx\nginx-<最新版本>` | configure 時 `-DTAIDAFLOW_NGINX_DIR=<資料夾>` |
| `scripts\verify-release-package.ps1` | | emsdk 的 node 16.20.0(只用來算網頁的下載連結) | 改那一行 |

(行號以本文件撰寫時的 `core` 為準。)

**建置腳本的環境變數**(不設 = 預設值;值寫資料夾,不要結尾反斜線):

| 環境變數 | 預設值 | 意思 | 用到的腳本 |
|---|---|---|---|
| `TAIDAFLOW_QT_ROOT` | `C:\Qt\6.8.3` | Qt 6.8.3 資料夾(底下要有 `msvc2022_64`、`wasm_singlethread`) | build-desktop、build-wasm |
| `TAIDAFLOW_QT_TOOLS` | `C:\Qt\Tools` | Qt Tools 資料夾(底下要有 `CMake_64\bin\cmake.exe`、`Ninja\ninja.exe`) | build-desktop、build-wasm |
| `TAIDAFLOW_VCVARS64` | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat` | vcvars64.bat 的完整路徑 | build-desktop |
| `TAIDAFLOW_EMSDK` | `C:\tools\emsdk` | emsdk 資料夾(已 install + activate 3.1.56) | build-wasm |
| `TAIDAFLOW_NO_PRESET` | (不設) | 設成 `1` = 就算路徑都是預設也不用 preset | build-desktop、build-wasm |

腳本的行為:

- 路徑都是預設值(且 build-wasm 另外確認 emsdk 裡有 `python\3.9.2-nuget_64bit\python.exe` 與
  `node\16.20.0_64bit\bin\node.exe`)→ 跟以前完全一樣:`cmake --preset <名稱>` + `cmake --build --preset <名稱>`。
- 任何一個路徑不是預設(或 `TAIDAFLOW_NO_PRESET=1`)→ 印出 `[build-desktop] tool paths from ... (no preset)` /
  `[build-wasm] ... (no preset)`,改用與 preset **相同的設定**但路徑取自環境變數的完整 `cmake -S . -B build\<名稱> ...`
  指令(§4.3 / §5.3 那種),輸出到**同一個** `build\desktop` / `build\wasm-release`。wasm 的 node 取自
  `emsdk_env.bat` 設的 `EMSDK_NODE`。
- 開始前逐一檢查 vcvars64.bat、cmake.exe、ninja.exe、Qt kit、emsdk 是否存在,缺哪個就印出路徑與該設哪個變數,exit 1。
- `build-wasm.bat` 的 preset 參數若不是 `wasm-release` / `wasm-debug`(例如自己在 `CMakeUserPresets.json` 定義的),
  一律當 preset 使用。
- 對已經建過的 `build\desktop` / `build\wasm-release` 換了工具路徑,第一次要加 `fresh`。

設定方式(二選一):

```bat
:: 只對目前這個 cmd 視窗有效
set TAIDAFLOW_QT_ROOT=D:\Qt\6.8.3
set TAIDAFLOW_QT_TOOLS=D:\Qt\Tools
set TAIDAFLOW_VCVARS64=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
set TAIDAFLOW_EMSDK=D:\emsdk
scripts\build-desktop.bat fresh
scripts\build-wasm.bat wasm-release fresh

:: 永久(之後新開的 cmd 才生效;或用「系統內容 → 環境變數」設定)
setx TAIDAFLOW_QT_ROOT D:\Qt\6.8.3
```

**直接用 preset(`cmake --preset`、Qt Creator)時**:CMake preset 的 JSON 不能寫「環境變數,沒設就用預設值」,
所以 `CMakePresets.json` 保持參考機的路徑不動。工具在別的位置時,在 repo 最上層建 `CMakeUserPresets.json`
(`.gitignore` 已排除,不會被提交),繼承原本的 preset、只覆寫路徑。範本(把 `D:/Qt`、`D:/emsdk` 換成自己的;
Node / Python 資料夾名稱照 `dir D:\emsdk\node`、`dir D:\emsdk\python` 看到的填):

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "my-desktop-release",
      "displayName": "Desktop Release (my tool paths)",
      "inherits": "desktop-release",
      "toolchainFile": "D:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/qt.toolchain.cmake",
      "cacheVariables": { "CMAKE_MAKE_PROGRAM": "D:/Qt/Tools/Ninja/ninja.exe" }
    },
    {
      "name": "my-wasm-release",
      "displayName": "WASM Release (my tool paths)",
      "inherits": "wasm-release",
      "toolchainFile": "D:/Qt/6.8.3/wasm_singlethread/lib/cmake/Qt6/qt.toolchain.cmake",
      "cacheVariables": {
        "CMAKE_MAKE_PROGRAM": "D:/Qt/Tools/Ninja/ninja.exe",
        "QT_CHAINLOAD_TOOLCHAIN_FILE": "D:/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake",
        "EMSCRIPTEN_ROOT_PATH": "D:/emsdk/upstream/emscripten",
        "QT_HOST_PATH": "D:/Qt/6.8.3/msvc2022_64",
        "QT_HOST_PATH_CMAKE_DIR": "D:/Qt/6.8.3/msvc2022_64/lib/cmake",
        "CMAKE_CROSSCOMPILING_EMULATOR": "D:/emsdk/node/16.20.0_64bit/bin/node.exe"
      },
      "environment": {
        "EMSDK": "D:/emsdk",
        "EM_CONFIG": "D:/emsdk/.emscripten",
        "EMSCRIPTEN": "D:/emsdk/upstream/emscripten",
        "EMSDK_PYTHON": "D:/emsdk/python/3.9.2-nuget_64bit/python.exe",
        "EMSDK_NODE": "D:/emsdk/node/16.20.0_64bit/bin/node.exe",
        "PATH": "D:/emsdk/python/3.9.2-nuget_64bit;D:/emsdk/node/16.20.0_64bit/bin;D:/emsdk/upstream/emscripten;D:/emsdk/upstream/bin;D:/Qt/6.8.3/msvc2022_64/bin;D:/Qt/Tools/Ninja;D:/Qt/Tools/CMake_64/bin;C:/Windows/System32;C:/Windows;C:/Windows/System32/Wbem;C:/Windows/System32/WindowsPowerShell/v1.0"
      }
    }
  ],
  "buildPresets": [
    { "name": "my-desktop-release", "configurePreset": "my-desktop-release", "targets": ["TaidaFlowApp"] },
    { "name": "my-wasm-release", "configurePreset": "my-wasm-release", "inheritConfigureEnvironment": true, "targets": ["TaidaFlowApp"] }
  ]
}
```

- 輸出資料夾繼承原 preset(`build\desktop`、`build\wasm-release`),其他腳本(`deploy-web.ps1`、`package-release.ps1`、
  檢查腳本)照常找得到。
- 用法:`cmake --preset my-desktop-release` + `cmake --build --preset my-desktop-release`(先 vcvars64,§4.2);
  網頁版也可以 `scripts\build-wasm.bat my-wasm-release`。
- 路徑用正斜線 `/`。`PATH` 是**整個取代**,不是附加(與原 preset 相同)。
- 範本的 Node / Python 路徑是 emsdk `git checkout 3.1.56` 時的樣子(`node/16.20.0_64bit/bin/node.exe`)。較新的 emsdk
  資料夾結構不同,例如 2026-09 的 emsdk 是 `node/24.19.0_64bit/node.exe`(**沒有 `bin`**)與 `python/3.13.3_64bit/python.exe`
  (實測);照實際檔案位置填,或乾脆用 `scripts\build-wasm.bat`(不需要 preset)。

### 2.7 全新電腦檢查清單

依序做完、每一項都符合預期再開始編譯:

- [ ] repo 在短路徑、無空白中文(§1.3),`git status` 顯示 `On branch core`
- [ ] `qtpaths.exe --qt-version` = `6.8.3`;兩個 kit 的 `lib\cmake` 模組各 7 / 4 行都在(§2.2)
- [ ] `cmake --version` = 3.30.x(≥ 3.25);`ninja --version` = 1.12.1(§2.2)
- [ ] `vcvars64.bat` 路徑已知;執行後 `cl` 印出 `for x64`(§2.3)
- [ ] `emcc --version` = `3.1.56`;emsdk 資料夾可寫入;`dir <emsdk>\node`、`dir <emsdk>\python` 看得到版本資料夾(§2.4)
- [ ] (選用)要重新產生網頁版字型才需要:`py -3 -c "import fontTools"` 成功(§2.5)
- [ ] 工具不在預設位置 → `TAIDAFLOW_*` 環境變數已設(§2.6);要用 preset / Qt Creator → `CMakeUserPresets.json` 已建
- [ ] `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-pack.ps1` exit 0(pack 24/24,§7)
- [ ] `scripts\build-desktop.bat` exit 0、`build\desktop\TaidaFlowApp.exe` 存在(§4)
- [ ] `scripts\build-wasm.bat` exit 0、`build\wasm-release\` 5 個網頁檔存在(§5)
- [ ] §7 的檢查全部 exit 0

---

## 3. 專案結構與 CMake 結構

```text
TaidaFlow/                 (repo,core 分支)
├─ CMakeLists.txt          根:平台判斷、find_package、加入 pack 與各子目錄
├─ CMakePresets.json       desktop-release / wasm-debug / wasm-release
├─ qds.cmake、cmake/       Qt Design Studio 產生(TaidaFlow、TaidaFlowContent、App、Dependencies)
├─ App/                    main.cpp、embeddedfonts.*、lanrelay.h、fonts/(內嵌字型子集)、wasm/(網頁載入頁)
├─ Core/                   TaidaFlowProxy.h(QML 的 Td)+ 後端(只有桌面版編):core、manager、Modbus_*、
│                          Ms300FaultReader、SqlManager、RESTManager、HistoryExport、HistoryViews、AppHttpServer/
├─ TaidaFlow/、TaidaFlowContent/、Dependencies/   QDS 的 QML 畫面與元件(main 分支維護)
├─ integration-pack/wasm-mirror/                  網頁同步套件 pack 1.0.1(唯讀)
├─ scripts/                建置、執行、檢查、打包腳本(PowerShell / 批次檔;只有字型子集是 Python,§2.5)
├─ deploy/                 nginx 設定樣板(nginx/)、正式機腳本(release/)、開發機設定檔(dev/config.dev.json、config.simulator.json)
├─ docs/                   本文件、DEPLOY_AND_STARTUP.md、整合報告、evidence/(各輪驗證證據)
├─ build/                  所有建置輸出(.gitignore 排除)
└─ dist/                   打包輸出(.gitignore 排除)
```

CMake 結構:

- **根 `CMakeLists.txt`**:先判斷 `EMSCRIPTEN` 再判斷 `WIN32`(`TAIDAFLOW_IS_WASM` / `TAIDAFLOW_IS_WINDOWS_DESKTOP`;
  其他平台直接 `FATAL_ERROR`)。兩邊共用 `Core Gui Widgets Qml Quick QuickTimeline ShaderTools Network WebSockets`;
  **只有桌面版**另外 `find_package` `SerialBus SerialPort Sql HttpServer Concurrent`。
  接著 `add_subdirectory(integration-pack/wasm-mirror)`、`Core`,最後由 `qds.cmake` 加入 QDS 的子目錄。
- **Desktop-only Core / WASM 只編 Proxy**(pack 文件 package-integration §1/§5.4):`Core/CMakeLists.txt` 兩邊共用的來源只有
  `TaidaFlowProxy.h`;後端 `.cpp` 與上述 Qt 模組只在桌面版加入。網頁版是 replica,資料全部經 mirror(8125)跟桌面版同步。
  `wasm_mirror_register_proxy(CLASS TaidaFlowProxy)` + `wasm_mirror_finalize_proxy_registration` 在 `Core` target 上,
  兩邊用同一份 contract。
- **integration-pack/wasm-mirror**:pack **1.0.1**(wire protocol 3,`VERSION.txt`),以子目錄加入;它自己在子目錄關掉
  `CMAKE_INCLUDE_CURRENT_DIR`(宿主不加 workaround,§9 的 VERSION 問題)。
- **App/wasm 載入頁**:Qt 6.8 沒有自訂 HTML shell 的參數,它在 **configure** 時產生 `build\<wasm>\TaidaFlowApp.html`。
  `App/CMakeLists.txt` 在同一次 configure、Qt 產生之後呼叫 `App/wasm/apply_wasm_shell.cmake`,用
  `App/wasm/TaidaFlowApp.shell.html` 蓋掉它;configure log 會印兩行 `[wasm-shell] ...`。只影響網頁版。
- 桌面版找網頁資料夾的最後一個候選是 `<exe 資料夾>\..\wasm-release`(`build\desktop` → `build\wasm-release`,開發機直接可用)。
  w2-062 起是**相對於 exe** 的路徑,不再把建置機的絕對路徑編進程式(打包資料夾不帶建置機路徑)。
- **網頁版的 QML 匯入掃描只看專案資料夾**(w2-062):Qt 6.8 在網頁版(靜態 Qt)的 configure 結尾用 `qmlimportscanner` 決定要靜態
  連結哪些 QML 模組,它預設以 target 的來源資料夾(= repo 最上層)為唯一 `-rootPath`,會掃到 `build\`、`dist\` 裡的 QML 檔
  (例如打包資料夾裡 windeployqt 複製的桌面版 Qt 模組),多連結用不到的模組。Qt 的 CMake 沒有公開參數可改這個範圍,
  `qmlimportscanner` 自己的 `-exclude` 對 `-rootPath` 掃描也無效(實測 Qt 6.8.3)。根 `CMakeLists.txt` 最後一段因此接手
  Qt 的內部函式 `_qt_internal_scan_qml_imports`:Qt 照常掃描後,用 Qt 寫好的同一份參數檔再掃一次,只把 `-rootPath` 換成 repo
  最上層的每個資料夾(不含 `build*`、`dist`、`cmake-build-*`、`.` 開頭的資料夾),結果取代 Qt 的。configure log 有一行
  `-- [qml-scan] QML import scan limited to the project folders (...): 45 import(s) (Qt's whole-folder scan: 59)`。
  Qt 版本不同、找不到那個函式或參數檔時只印 warning 並沿用 Qt 自己的結果。只影響網頁版。
- **桌面版建置同時產生 nginx 資料夾**(w2-062,§4.5):根 `CMakeLists.txt` 的 target `taidaflow_nginx_conf`(`TaidaFlowApp`
  相依於它)產生 `build\desktop\nginx\`。

---

## 4. 桌面版編譯

輸出:`build\desktop\TaidaFlowApp.exe`(參考機 3,426,304 bytes;專案的 QML 模組靜態連結進 exe)。
參考機完整編譯約 40 秒(578 個 Ninja 步驟)。

### 4.1 用腳本

```bat
scripts\build-desktop.bat            :: 增量
scripts\build-desktop.bat fresh      :: 先刪 build\desktop 再從頭建
```

腳本做的事:檢查工具 → `vcvars64.bat` → `cmake --preset desktop-release` → `cmake --build --preset desktop-release`
(工具不在預設位置時改用 §4.3 的指令,§2.6)。exit 0 = 成功。

### 4.2 手動 A:用 preset(不用腳本)

開一個**新的 cmd**(不是 PowerShell),逐行輸入:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\src\TaidaFlow
C:\Qt\Tools\CMake_64\bin\cmake.exe --preset desktop-release
C:\Qt\Tools\CMake_64\bin\cmake.exe --build --preset desktop-release
```

- 第 1 行換成自己的 vcvars64.bat(§2.3),第 2 行換成自己的 repo 路徑。
- 輸出在 `build\desktop`(preset 的 `binaryDir`)。想先建在別的資料夾試:第 3 行加 `-B build\<名稱>`,第 4 行改成
  `C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\<名稱> --target TaidaFlowApp`(build preset 固定對 `build\desktop`)。
- `vcvars64` 一定要先跑,而且要在**同一個** cmd 視窗;不然 CMake 找不到 `cl.exe`。

### 4.3 手動 B:不用 preset(完整 cmake 指令)

與 `desktop-release` preset 相同的設定,全部寫在命令列(工具路徑依實際位置改):

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\src\TaidaFlow
set VSLANG=1033
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build\desktop -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/qt.toolchain.cmake
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\desktop --target TaidaFlowApp
```

- `set VSLANG=1033`:讓編譯器訊息用英文(preset 也這樣設),可省略。
- `--target TaidaFlowApp`:只建 app(與 build preset 相同);不加會多建 QDS 的其他 target。
- 本輪驗證把 `build\desktop` 換成 `build\manual-desktop`(不動既有的 `build\desktop`),其餘一字不差。

### 4.4 執行

- exe 需要 Qt 的 DLL:開發機上把 `C:\Qt\6.8.3\msvc2022_64\bin` 加到 PATH;**一律用 `scripts\run-desktop.ps1` 啟動**
  (先做安全探測,因為 app 一啟動就會連 config.json 裡的 ADAM 位址並寫入輸出)。設定檔用 `deploy\dev\config.dev.json`
  (§10;`run-desktop.ps1` 以環境變數 `TAIDAFLOW_CONFIG` 指定,`build\desktop` 旁邊**不會**產生 config.json)。
  詳見 README「安全注意」與 [DEPLOY_AND_STARTUP.md §9](DEPLOY_AND_STARTUP.md)。
- 正式機用打包資料夾(`scripts\package-release.ps1`,需要 `build\desktop` 與 `build\wasm-release` 都是最新)。

### 4.5 桌面版建置產生的 nginx 資料夾(`build\desktop\nginx`)

桌面版每次建置(不論用腳本、preset 或手動 cmake;網頁版不做)都會一起產生一個可以直接啟動的 nginx 資料夾,
和正式機的用法相同(`cd` 到 nginx 資料夾,`start nginx`):

```text
build\desktop\nginx\
├─ nginx.exe            從 TAIDAFLOW_NGINX_DIR 複製
├─ conf\mime.types      從 TAIDAFLOW_NGINX_DIR\conf 複製
├─ conf\nginx.conf      產生(TaidaFlow 的完整設定)
├─ logs\、temp\         建立(nginx 自己的 log 與暫存)
```

- `conf\nginx.conf` 由 `scripts\install-nginx-config.ps1 -Build` 產生——與正式機一次性安裝步驟**同一支產生程式**
  (模板 `deploy\nginx\taidaflow.conf`)。值來自快取變數 `TAIDAFLOW_NGINX_CONFIG` 指定的設定檔
  (預設 `deploy\dev\config.dev.json`):監聽 `nginx.port`、網頁根目錄 `build\desktop\web`(寫成相對於 nginx 資料夾的
  `../web`)、`/exports` = `<dataDir>\exports`、`/api/` → `127.0.0.1:<rest.port>`、`/mirror` → `127.0.0.1:<mirror.internalPort>`。
  產生後在該資料夾執行 `nginx -t`(失敗時建置失敗)。模板、產生程式或設定檔有改,下次建置會自動重新產生
  (建置輸出直接取代,不留 `.prev-<時間>` 備份;正式機的 `install-nginx-config.ps1` 才會留備份)。
- 兩個 CMake 快取變數(configure 時用 `-D` 指定,或在 Qt Creator 的 CMake 設定改):

  | 變數 | 預設 | 意思 |
  |---|---|---|
  | `TAIDAFLOW_NGINX_DIR` | `C:\tools\nginx\nginx-<最新版本>`(configure 時找) | 要複製 `nginx.exe` 與 `conf\mime.types` 的 nginx for Windows 資料夾 |
  | `TAIDAFLOW_NGINX_CONFIG` | `<repo>\deploy\dev\config.dev.json` | 產生 `nginx.conf` 用的 config.json |

- 找不到 nginx(`TAIDAFLOW_NGINX_DIR` 不存在或沒有 `nginx.exe`):configure 印 `CMake Warning ... [nginx] ...`,只產生
  `conf\nginx.conf`(不跑 `nginx -t`),**建置照常成功**。
- 開發機使用:先部署網頁(`scripts\deploy-web.ps1` → `build\desktop\web`),再
  ```bat
  cd /d <repo>\build\desktop\nginx
  start nginx
  nginx -s reload     :: 設定重新產生後套用
  nginx -s quit       :: 停止(同一個資料夾)
  ```
  也可以用 `scripts\nginx-start.ps1` / `nginx-stop.ps1`(開發用,另一個獨立的 `build\nginx` 前綴,兩者擇一,不要同時開在同一個 port)。
- 這份 `nginx.conf` **只適用建置機**(匯出資料夾是 config.dev.json 的 `build\runtime-cwd\exports`)。正式機的 nginx 設定一律在
  目標電腦上重新產生(`install-nginx-config.ps1`,或 `start-taidaflow` 發現不符時自動重產,見 DEPLOY_AND_STARTUP.md)。
  `package-release.ps1` 打包時附上 nginx 程式,但**不附** `nginx.conf`。
- 產生的都是建置輸出(`build\` 已被 git 忽略),不寫進原始碼樹,也不動 nginx 安裝資料夾。

---

## 5. 網頁版編譯(WebAssembly)

輸出(`build\wasm-release\`,網頁只需要這 5 個檔;其他 CMake / Ninja 檔不要部署):

| 檔案 | 參考機大小(bytes) |
|---|---|
| `TaidaFlowApp.html`(TaidaFlow 載入頁,§5.5) | 12,155 |
| `TaidaFlowApp.js` | 304,976 |
| `TaidaFlowApp.wasm` | 33,847,174(約 34 MB;`deploy-web.ps1` 產生的 `.gz` 約 12.4 MB)。w2-062 起 QML 匯入掃描只看專案資料夾(§3、§9),repo 裡有沒有 `dist\`、`build\` 大小都一樣(w2-062 實測:工作目錄與不含 `dist\`/`build\` 的乾淨複本都是 33,847,174,只差 rcc 時間戳記的 69 個 byte);w2-062 之前參考機曾因掃到 `dist\` 而多連結 Windows 原生樣式模組 |
| `qtloader.js` | 12,354 |
| `qtlogo.svg`(新載入頁不用,留著無影響) | 1,686 |

參考機完整編譯約 3 分鐘(568 個步驟;最後的連結 + 最佳化最久,畫面停在 `Linking CXX executable TaidaFlowApp.js`
好幾分鐘是正常的)。

### 5.1 用腳本

```bat
scripts\build-wasm.bat                        :: wasm-release,增量
scripts\build-wasm.bat wasm-release fresh     :: 從頭建
scripts\build-wasm.bat wasm-debug             :: Debug 版 -> build\wasm-debug
```

腳本做的事:檢查工具 → `emsdk_env.bat` → `cmake --preset <preset>` → `cmake --build --preset <preset>`
(工具不在預設位置或 emsdk 的 Python/Node 版本不同時改用 §5.3 的設定,§2.6)。不需要 vcvars64。

### 5.2 手動 A:用 preset

開一個**新的 cmd**(不要用剛跑過 vcvars64 的視窗):

```bat
call C:\tools\emsdk\emsdk_env.bat
cd /d C:\src\TaidaFlow
C:\Qt\Tools\CMake_64\bin\cmake.exe --preset wasm-release
C:\Qt\Tools\CMake_64\bin\cmake.exe --build --preset wasm-release
```

- preset 自己帶了 emsdk 的環境(`environment`),嚴格說第 1 行可省略;保留它可以先確認 emsdk 正常。
- preset 內寫的是 `C:\tools\emsdk` 與 node 16.20.0 / python 3.9.2-nuget;不同時用 §2.6 的 `CMakeUserPresets.json`。
- 輸出在 `build\wasm-release`;要建到別的資料夾同 §4.2(第 3 行加 `-B`、第 4 行 `--build build\<名稱> --target TaidaFlowApp`)。

### 5.3 手動 B:不用 preset(qt-cmake)

```bat
call C:\tools\emsdk\emsdk_env.bat
emcc --version
cd /d C:\src\TaidaFlow
call C:\Qt\6.8.3\wasm_singlethread\bin\qt-cmake.bat -S . -B build\wasm-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DQT_HOST_PATH=C:/Qt/6.8.3/msvc2022_64 -DQT_HOST_PATH_CMAKE_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake -DBUILD_TESTING=OFF
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\wasm-release --target TaidaFlowApp
```

- `qt-cmake.bat` 是 Qt 附的包裝:幫你加上 wasm kit 的 `CMAKE_TOOLCHAIN_FILE`,並依 `emsdk_env.bat` 設的 `EMSDK`
  找到 Emscripten 的 toolchain(所以一定要先跑第 1 行)。在 .bat 檔裡呼叫另一個 .bat 要加 `call`,直接在 cmd 輸入加不加都可以。
- **`QT_HOST_PATH` 與 `QT_HOST_PATH_CMAKE_DIR` 兩個都要給**:wasm kit 記錄的預設 host 是 `C:/Qt/6.8.3/mingw_64`
  (Qt 官方建置時的設定)。只給 `QT_HOST_PATH` 時,若電腦上剛好有 `mingw_64`,`QT_HOST_PATH_CMAKE_DIR` 會被設成
  mingw 的(本輪第一次手動實測就是這樣,見 `docs/evidence/w2-063/d2-02a-*`);沒裝 mingw_64 又都不給時 configure 會失敗
  (`please set the QT_HOST_PATH cache variable`)。
- `-DCMAKE_CROSSCOMPILING_EMULATOR` 沒給時 CMake 會找 PATH 上任何一個 node(只在跑測試時用,不影響產物);
  要與 preset 完全一樣可加 `"-DCMAKE_CROSSCOMPILING_EMULATOR=%EMSDK_NODE%"`。
- 本輪驗證把 `build\wasm-release` 換成 `build\manual-wasm`,其餘一字不差。

### 5.4 部署與開啟

`powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1` 把 `build\wasm-release` 的網頁檔複製到
`build\desktop\web\` 並產生 `.gz`;網頁由桌面版(8124)或 nginx(80)提供,**不能用 file:// 直接開**。
細節見 [DEPLOY_AND_STARTUP.md §2](DEPLOY_AND_STARTUP.md)。

### 5.5 載入頁(configure 時套用)

- 樣板 `App/wasm/TaidaFlowApp.shell.html`,套用腳本 `App/wasm/apply_wasm_shell.cmake`(§3)。
- configure log 應出現:
  `-- [wasm-shell] <build>/TaidaFlowApp.html <- <repo>/App/wasm/TaidaFlowApp.shell.html` 與
  `-- [wasm-shell] APPNAME=TaidaFlowApp APPEXPORTNAME=TaidaFlowApp_entry PRELOAD='' (...) sha256=...`。
- 改了樣板:重新跑 `scripts\build-wasm.bat`(樣板是 configure 相依,會自動重跑 configure)→ 重新 `deploy-web.ps1`。
- 靜態檢查(不開瀏覽器):`findstr /c:"TAIDAFLOW" build\wasm-release\TaidaFlowApp.html` 有結果、`findstr /c:"@APPNAME@"
  build\wasm-release\TaidaFlowApp.html` 沒有結果(佔位都已填入)。

---

## 6. 用 Qt Creator 開專案

> 本節只有說明,**沒有實測**(本輪不做 UI 操作)。參考機的 Qt Creator 是 20.0.1。

1. Qt Creator → **File → Open File or Project** → 選 repo 最上層的 `CMakeLists.txt`(不是 `TaidaFlow.qmlproject`,
   那是 Qt Design Studio 用的 UI 專案)。
2. **Configure Project** 頁面有兩種選法,擇一:
   - **用 Qt 的 kit(建議)**:勾 `Desktop Qt 6.8.3 MSVC2022 64bit`(Qt Creator 會自動偵測 MSVC;沒出現時到
     Preferences → Kits 確認 Qt 版本與編譯器),Build 設定選 **Release**。
     網頁版:先到 Preferences → Devices → **WebAssembly** 把 emsdk 路徑設成 `C:\tools\emsdk`(畫面會顯示偵測到的版本,
     必須是 3.1.56;沒有這一頁時到 Help → About Plugins 啟用 WebAssembly 外掛並重開),再勾
     `WebAssembly Qt 6.8.3 (single-threaded)`,並在 Projects → Build → CMake 的 **Initial Configuration** 加上
     `QT_HOST_PATH=C:/Qt/6.8.3/msvc2022_64`、`QT_HOST_PATH_CMAKE_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake`、
     `BUILD_TESTING=OFF`(原因同 §5.3),然後 **Re-configure with Initial Parameters**。
   - **用 CMakePresets.json 的 preset**:Qt Creator 會把 `desktop-release`、`wasm-release`、`wasm-debug`(以及
     `CMakeUserPresets.json` 的)列成可選的設定。工具不在預設位置時要先建 `CMakeUserPresets.json`(§2.6)。
3. 注意事項:
   - Qt Creator 預設的建置資料夾是 `build\Desktop_Qt_6_8_3_MSVC2022_64bit-Release` 之類(在 `build\` 下,已被忽略)。
     `deploy-web.ps1`、`package-release.ps1`、檢查腳本預設看 `build\desktop` / `build\wasm-release`;
     要打包或部署時請用腳本建一次,或把路徑當參數傳給檢查腳本。
   - **不要在 Qt Creator 直接按 Run 執行桌面版**:它不經過安全探測,app 一啟動就會連 `192.168.1.201~205` 的 ADAM 並寫入輸出。
     開發機上一律用 `scripts\run-desktop.ps1`(或 `-DeviceProfile simulator` 接模擬器,見 README)。
   - Qt Creator 執行網頁版時只會開一個獨立的測試網頁伺服器,沒有桌面版可以同步,畫面會顯示「離線」;
     網頁版要照 §5.4 由桌面版或 nginx 提供。
   - 同一個建置資料夾不要同時給 Qt Creator 與腳本 / 命令列用不同設定(會互相觸發重新 configure);要換就 `fresh`。

---

## 7. 編譯後必跑的檢查

全部在 repo 資料夾的 cmd 執行,以 **exit code** 判定(0 = 通過)。

以下 `PS` 代表 `powershell -NoProfile -ExecutionPolicy Bypass -File`(w2-062 起檢查腳本全部是 PowerShell,取代原本的 `.py`;
輸出與判定和原本相同,新舊版對同一建置的輸出逐行相同,證據 `docs/evidence/w2-062/05-python-to-ps1-equivalence/`)。

| 時機 | 指令 | 通過條件 |
|---|---|---|
| clone 後 / 更新 pack 後 | `PS scripts\verify-pack.ps1`(與來源比對:`-Source <pack 來源資料夾>`) | `MANIFEST` 24/24、exit 0 |
| 每次兩邊都建完 | `PS scripts\check-wasm-backend.ps1 build\desktop build\wasm-release` | 桌面 `verdict: OK`(9 個後端來源都在)、wasm `backend_sources (0)`、`backend_qt_libs (0)`、`backend_strings (0)`、`check_wasm_backend exit=0` |
| 每次兩邊都建完 | `PS scripts\check-version-shadow.ps1 build\desktop build\wasm-release` | 兩行都是 `version_shadow_hits=0`,exit 0 |
| 改過 UI / C++ 中文字串後(選用工具,§2.5) | `PS scripts\make-font-subset.ps1 --check` | exit 0;exit 1 = 有新字 → 照 §2.5 (B) 重新產生 → 重建 wasm |
| 改過 REST 相關程式後 | `PS scripts\check-rest-routes.ps1` | exit 0(log 的 route 表與實際註冊一致) |
| 改過 `Core/AppHttpServer/` 後(或定期) | `scripts\run-apphttpserver-tests.bat` | CTest `100% tests passed`,exit 0(需約 2 GiB 暫存磁碟空間;輸出 `build\apphttpserver-qtest`) |
| 改過 `App/appconfig.*`、`App/runtimeinfo.*` 後 | 建置並執行 `App/tests`(CTest,見 `App/tests/README.md`) | `100% tests passed` |
| 改過 pack 或升級 Qt 後 | `scripts\run-pack-tests.bat` | exit 0(pack 自帶的 native tests;輸出 `build\pack-tests`) |
| 改過 SqlManager / HistoryExport / HistoryViews / Proxy 後 | `docs\evidence\w2-062\tools\make-bench-db.bat`(C++ 測試資料產生器)→ `docs\evidence\w2-049\tools\run-w2041-qtest.bat`(需 8124 空著)、`docs\evidence\w2-045\tools\run-qtest.bat` 與 `docs\evidence\w2-052\tools\run-qtest.bat` | 各自 exit 0(資料量大,需要較長時間) |

- 檢查腳本的參數可以換成其他建置資料夾(例如 Qt Creator 的),桌面版與網頁版各給一個。
- 需要啟動 app 的整合檢查(`verify-desktop-startup.ps1`、nginx、REST、打包驗證)見 README「測試 / 驗證」。

---

## 8. 分支分工(main = UI、core = 後端)與 main 合併進 core 之後

- UI(QML 畫面、`TaidaFlowProxy.h` 的介面)在 `main` 開發;後端、網頁版整合、腳本與文件在 `core`。
  UI 進 `main` 後合併到 `core`:`git switch core` → `git merge main`(推送前依團隊規定先給負責人看過)。
- `main` 沒有 `scripts\`、`CMakePresets.json`、`App/wasm/`、`integration-pack/`;`main` 的網頁版用 Qt 的
  `wasm_multithread` kit 與 Qt 預設載入頁。合併時這些只在 `core` 的檔案不會被動到,除非 `main` 也改了同一段
  (特別注意 `App/CMakeLists.txt` 的 w2-058 區塊、`Core/TaidaFlowProxy.h`、根 `CMakeLists.txt` 的平台判斷)。
- **合併後必做**(依序,全部 exit 0 才算合併完成):
  1. 字型:UI 常帶新的中文字。有 Python + fonttools 的人跑 `PS scripts\make-font-subset.ps1 --check`;exit 1 → 照 §2.5 (B)
     重新產生 → 把 `App/fonts/` 的三個檔一起提交。沒有 Python 的人:把網頁版有方框的情況回報給有工具的人處理。
  2. **fresh 建置**:`scripts\build-desktop.bat fresh`,再 `scripts\build-wasm.bat wasm-release fresh`
     (QDS 產生的 CMake / qmldir / 新 QML 檔與 configure 時套用的載入頁,都需要重新 configure 才會完全反映)。
  3. `PS scripts\check-wasm-backend.ps1 build\desktop build\wasm-release`
  4. `PS scripts\check-version-shadow.ps1 build\desktop build\wasm-release`
  5. `PS scripts\check-rest-routes.ps1`
  6. 改到 `TaidaFlowProxy.h` 時:§7 最後一列的 QTest,以及 README「測試 / 驗證」的啟動檢查(mirror 同步需要兩端同一份
     contract,桌面版與網頁版要一起重建、一起部署)。

---

## 9. 常見問題

**`#include <version>` 相關的奇怪編譯錯誤 / 標準函式庫錯誤**:pack 1.0.0 在最上層放了沒有副檔名的 `VERSION` 檔,
QDS 預設的 `CMAKE_INCLUDE_CURRENT_DIR ON` 讓它進了 include 路徑,Windows 不分大小寫,`#include <version>` 就讀到它。
pack 1.0.1 已改名 `VERSION.txt` 並在自己的子目錄關掉該設定。出現時:確認 `integration-pack\wasm-mirror\VERSION`(無副檔名)
**不存在**(`scripts\verify-pack.ps1` 會檢查)→ fresh 建置 → `scripts\check-version-shadow.ps1` 應為 `version_shadow_hits=0`。
不要把任何名為 `VERSION`(或 `version`)的檔案放在會進 include 路徑的資料夾。

**改了 `CMakePresets.json` 但 `git status` 看不到 / `git add` 沒反應**:`.gitignore` 有 `/CMakePresets.json`;
它已被追蹤,修改會顯示,但新加入要 `git add -f CMakePresets.json`。個人路徑請寫在 `CMakeUserPresets.json`(§2.6)。

**`Qt Wasm built with Emscripten version: 3.1.56` / `You are using Emscripten version: ...` / `This may not work correctly`**:
emsdk 版本不對。到 emsdk 資料夾 `emsdk install 3.1.56`、`emsdk activate 3.1.56`,重開 cmd 跑 `emsdk_env.bat`,
`emcc --version` 確認後 `scripts\build-wasm.bat wasm-release fresh`。

**`Cannot find the toolchain file Emscripten.cmake` / `emcc` 不是內部或外部命令**:沒先跑 `emsdk_env.bat`(手動做法),
或 `TAIDAFLOW_EMSDK` 指錯、emsdk 沒 install/activate 3.1.56。

**`please set the QT_HOST_PATH cache variable`**:手動 / Qt Creator 建網頁版時沒給 `QT_HOST_PATH`(§5.3、§6)。

**preset 找不到 node / python(路徑含 `16.20.0_64bit`、`3.9.2-nuget_64bit`)**:emsdk 不是 3.1.56 那一版的 emsdk,
帶的 Node / Python 版本不同。`scripts\build-wasm.bat` 會自動改用不經 preset 的做法;直接用 preset 時照 §2.4 把 emsdk
`git checkout 3.1.56` 後重新 install,或建 `CMakeUserPresets.json` 填實際的資料夾名稱。

**`No CMAKE_CXX_COMPILER could be found` / 找不到 `cl.exe`(桌面版)**:沒在同一個 cmd 先跑 `vcvars64.bat`,
或 VS 沒裝「使用 C++ 的桌面開發」。`scripts\build-desktop.bat` 找不到 vcvars64 會直接說要設 `TAIDAFLOW_VCVARS64`。

**跑 `vcvars64.bat` 時出現 `'vswhere.exe' is not recognized ...`**:參考機上也會出現,不影響(之後 `cl` 正常即可)。

**`Could not find a package configuration file provided by "Qt6HttpServer"`(或 SerialBus、SerialPort、WebSockets、
QuickTimeline、ShaderTools)**:Qt 安裝時沒勾該模組(§2.2),用 Qt Maintenance Tool 補裝。網頁版執行時畫面空白且
瀏覽器 console 有 `module "Qt5Compat.GraphicalEffects" is not installed` 之類訊息:WebAssembly kit 缺 Qt 5 Compatibility Module。

**執行 exe 時「找不到 Qt6Core.dll」等(0xc0000135)**:Qt 的 DLL 不在 PATH。開發機用 `scripts\run-desktop.ps1`(會加
`C:\Qt\6.8.3\msvc2022_64\bin`);手動時 `set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%`;正式機用打包資料夾(DLL 都在裡面)。

**什麼時候要 fresh 建置**:換了工具路徑 / Qt / emsdk / VS 版本;`main` 合併進 `core` 後;改了 `CMakeLists.txt`
的平台判斷或 `find_package`;pack 更新;出現 CMake cache 相關錯誤(例如 toolchain 改變、`CMAKE_MAKE_PROGRAM` 不對);
或建置結果怪怪的。一般改 `.cpp` / `.qml` 用增量即可。

**網頁載入頁沒更新 / 還是 Qt 預設頁**:載入頁是在 **configure** 時套用的(§5.5)。確認 configure log 有 `[wasm-shell]`
兩行;沒有就 `scripts\build-wasm.bat wasm-release fresh`。之後一定要重新 `deploy-web.ps1`(舊的 `web\` 與 `.gz` 會蓋掉新的),
瀏覽器重新整理。

**網頁版連結時出現大量 `wasm-ld: error: duplicate symbol: QAreaSeries::...`(`libQt6Charts.a` 與 `libQt6Graphs.a`)**:
Qt 的 `qmlimportscanner` 會掃描 QML 檔,決定要把哪些 QML 模組靜態連結進網頁版。Qt 預設掃描**整個 repo 資料夾**;w2-062 起
根 `CMakeLists.txt` 把範圍限制在 repo 最上層的專案資料夾(不含 `build*`、`dist`,§3 最後一點,configure log 的 `[qml-scan]`),
所以 `build\`、`dist\` 裡的 QML(另一份建置、打包資料夾裡 windeployqt 複製的 Qt 模組、指到 Qt 安裝資料夾的 junction)
不再影響網頁版。仍會掃到的是**專案資料夾本身**(`App`、`Core`、`TaidaFlow`、`TaidaFlowContent`、`Dependencies`、`docs`、
`scripts` …):不要把別的 QML 來源(另一份 clone、解壓的 Qt 範例、junction)放進這些資料夾;放了就移走,再
`scripts\build-wasm.bat wasm-release fresh`。configure log 沒有 `[qml-scan]` 那一行而有 `[qml-scan]` 的 warning 時,表示這個
Qt 版本的內部函式不同,掃描又回到整個 repo(w2-063 的實測:`build\` 下有指到 `C:\Qt` 的 junction 時,匯入從 59 個變成 459 個)。

**`Filename longer than 260 characters` / 奇怪的找不到檔案**:repo 路徑太長(§1.3),搬到 `C:\src\TaidaFlow` 之類再 fresh 建置。

**`emsdk_env.bat` 失敗或權限錯誤**:emsdk 資料夾不可寫(例如放在 `C:\Program Files`),搬到 `C:\tools\emsdk`。

**網頁版 configure 出現一堆 `CMake Warning`(`Qt6ProtobufTools could not be found because dependency WrapProtoc
could not be found`、`Qt policy QTP0001/QTP0004 is not set`、`Manually-specified variables were not used by the project:
BUILD_TESTING`)**:正常,參考機每次都有,不影響結果;以 exit code 與 `[wasm-shell]` 兩行為準。

**字型子集工具出現 `ModuleNotFoundError: No module named 'fontTools'` 或 `make-font-subset: no Python 3 found`(exit 9)**:
只影響重新產生網頁版字型(§2.5 (B)),編譯不受影響。照 §2.5 安裝後再跑。

**桌面版 configure 出現 `CMake Warning ... [nginx] TAIDAFLOW_NGINX_DIR="..." holds no nginx.exe`**:這台電腦沒有 nginx
(或放在別處)。建置照常成功,只是 `build\desktop\nginx` 沒有 `nginx.exe`。要用時裝 nginx for Windows 到
`C:\tools\nginx\nginx-<版本>\`,或 configure 時加 `-DTAIDAFLOW_NGINX_DIR=<資料夾>`(§4.5)。

---

## 10. config.json(設定檔)

桌面版所有「現場會不一樣」的設定(資料資料夾、5 台 ADAM 與 MS300 的位址 / 序列埠參數、各服務的位址與 port、nginx)
都在 **`config.json`**(w2-061 讀取器 `App/appconfig.*`、w2-062 後端與腳本接上)。欄位表與正式機用法見
[DEPLOY_AND_STARTUP.md §6](DEPLOY_AND_STARTUP.md);這裡只寫開發者要知道的部分。

- **程式找檔的順序**:環境變數 `TAIDAFLOW_CONFIG`(完整路徑)→ `<TaidaFlowApp.exe 所在資料夾>\config.json` →
  兩者都沒有時**在 exe 旁建立**預設檔。JSON 格式錯誤:程式顯示錯誤對話框並結束(exit 2),**不覆寫**檔案。
  `TaidaFlowApp.exe --write-default-config <路徑>` 只寫出預設檔就結束(已存在不覆寫,exit 3)。
- **開發機一律用 repo 內的開發設定**,不要讓 `build\desktop` 旁邊出現 config.json(那會是正式機預設:資料寫到
  `C:\TaidaFlowData`、連廠區 IP):
  | 檔案 | 內容 | 用法 |
  |---|---|---|
  | `deploy\dev\config.dev.json` | `dataDir` = `../../build/runtime-cwd`(相對於設定檔所在資料夾)、設備位址維持廠區預設、nginx.exe 指向開發機的 nginx | `scripts\run-desktop.ps1` 的預設;接模擬器時 `-DeviceProfile simulator`(`TAIDAFLOW_DEVICE_PROFILE=simulator` 把 5 台 ADAM 換成 `127.0.0.201~205`,log 會記) |
  | `deploy\dev\config.simulator.json` | 同上,但 5 台 ADAM 直接寫 `127.0.0.201~205` | `scripts\run-desktop.ps1 -Config deploy\dev\config.simulator.json` |
  `run-desktop.ps1` 以 `TAIDAFLOW_CONFIG` 把設定檔交給程式,設定檔不存在就拒絕啟動;`safety_probe.ps1`、`nginx-web.ps1`、
  `deploy-web.ps1`、`verify-desktop-startup.ps1` 用同一份(`-Config`,否則 `TAIDAFLOW_CONFIG`,否則 `config.dev.json`)。
  改自己的 port 或 nginx 位置:複製一份 `config.dev.json` 到 `build\` 底下改,再用 `-Config` 指定(不要改到別人的設定)。
- **腳本不重複定義預設值**:`scripts\taidaflow-config.ps1`(所有腳本共用的讀取器)缺檔時呼叫
  `TaidaFlowApp.exe --write-default-config` 產生;檔案缺某些鍵時,也從同一個程式取得預設值。驗證規則(型別、範圍)與
  `App/appconfig.cpp` 相同。唯一例外是 `nginx.exe`(只有腳本用;程式的讀取器之後由 main 分支補上):檔案沒有時用
  `nginx\nginx.exe`(相對於 config.json 所在資料夾)。
- **環境變數**:`TAIDAFLOW_REST_PORT` 已移除(REST port 改由 `rest.port`);`TAIDAFLOW_DOWNLOAD_PORT` 仍可臨時覆寫下載連結的
  port(程式 log 會記「environment override」);`TAIDAFLOW_DEVICE_PROFILE=simulator` 只給測試用。
- **打包**:`package-release.ps1` 不附 config.json(正式機第一次啟動時建立,之後更新版本不會覆蓋);打包資料夾的
  `web\runtime.json` 用程式預設值產生,正式機的程式每次啟動都會依它的 config.json 重寫。
- **固定在程式裡、不放 config.json**(Mango 決定):Modbus TCP 逾時 1000 ms / 重試 2 / 重連 3000 ms、MS300 逾時 1000 ms /
  重試 1 / 輪詢 1000 ms。
- 程式啟動時 log 會列出每一項的生效值與來源(`[Config] ...`:file / default / environment;後端使用的值另有
  `[Config] Core ...` 行)。

---

## 11. 驗證紀錄與沒有驗證到的部分

2026-09-28(w2-063)在參考開發機上實際執行,exit code 與 log 在 `docs/evidence/w2-063/`:

| 項目 | 怎麼驗證 |
|---|---|
| §4.3 桌面手動 B | 照打(輸出 `build\manual-desktop`),vcvars64 / configure / build 各 exit 0 |
| §5.3 網頁手動 B | 照打(輸出 `build\manual-wasm`),emsdk_env / emcc / qt-cmake / build 各 exit 0;第一次只給 `QT_HOST_PATH` 時 `QT_HOST_PATH_CMAKE_DIR` 變成 mingw_64(另有只做 configure 的重現)→ 本文件已改成兩個都給,重跑 exit 0 |
| §7 檢查 | `check_wasm_backend`、`check_version_shadow`(對 `build\manual-*`)、`make_font_subset --check`、`check_rest_routes`、`verify_pack`(以 `py -3 -B` 執行,§2.5)、`run-apphttpserver-tests.bat`、`run-pack-tests.bat` 各 exit 0 |
| §1.1 clone | 真的從 GitHub:`git clone -b core` → 在 `core`;`git clone` 後 `git checkout core` 失敗(與 `Core\` 撞名)、`git switch core` 成功;再把 clone 帶到本輪 HEAD 與本輪修改的檔案 |
| §4.1 / §5.1 / §4.2 / §5.2 預設路徑 | 在上述 clone 資料夾(不同的 repo 位置)跑 `build-desktop.bat`、`build-wasm.bat`(preset 做法)與手動 A,各 exit 0 |
| §2.6 不同安裝位置 | 在上述 clone:Qt、Qt Tools、VS 用 junction 放到另一個路徑、emsdk 用**另外真的安裝的一份**(下一列),設 `TAIDAFLOW_*` 後 `build-desktop.bat fresh`、`build-wasm.bat wasm-release fresh` 各 exit 0,`CMakeCache.txt` 用的都是新路徑;`CMakeUserPresets.json` 範本(照抄本文件,只換路徑)建置桌面版與網頁版 exit 0;工具路徑錯誤時腳本印出訊息並 exit 1(5 種情況) |
| §2.4 emsdk 安裝 | 另一個資料夾真的 `git clone` emsdk 並 `emsdk install / activate 3.1.56`(不加 `--permanent`,使用者永久環境變數前後都沒有 `EMSDK`):(a) 2026-09 最新 emsdk → Node 24.19.0 / Python 3.13.3,`emcc` 3.1.56,用它建網頁版 exit 0(腳本自動不經 preset);(b) `git checkout 3.1.56` → Node 16.20.0 / Python 3.9.2-nuget / Java 8,與 `CMakePresets.json` 一致,用 `CMakeUserPresets.json` 建網頁版 exit 0。第一次 install 因 cmd 的 `python` 是 Store 別名而失敗,照 §2.4.2 的做法後成功 |

2026-09-28(w2-062)在參考開發機上實際執行,log 在 `docs/evidence/w2-062/`:

| 項目 | 結果 |
|---|---|
| §4.1 / §5.1 全新建置 | `build-desktop.bat fresh`、`build-wasm.bat wasm-release fresh` 各 exit 0;網頁版 configure 印 `[qml-scan] ... 45 import(s) (Qt's whole-folder scan: 59)` |
| §3 QML 匯入掃描(D6) | 工作目錄(有 `dist\`、`build\`)與不含它們的乾淨複本建出的 `TaidaFlowApp.wasm` 都是 33,847,174 bytes |
| §4.5 nginx 資料夾 | 全新建置產生、`nginx -t` 通過;改模板、產生程式或 config.dev.json(只改時間也算)→ 下次建置重新產生,沒改 → `no work to do`;`-DTAIDAFLOW_NGINX_DIR=<不存在>` → configure 警告、建置 exit 0、只有 `conf\`;還原後 `nginx.exe` 回來 |
| §7 檢查 | `check-wasm-backend.ps1`、`check-version-shadow.ps1`、`check-rest-routes.ps1`、`verify-pack.ps1`、`run-apphttpserver-tests.bat`、`App/tests`(CTest)、bench(`make-bench-db.bat` → w2-041/w2-049、w2-045、w2-052、w2-053 的 QTest)各 exit 0;新舊(`.py` / `.ps1`)檢查輸出逐行相同後才刪 `.py` |
| §2.5 字型 | `make-font-subset.ps1 --check` exit 1:main 的 `App/appconfig.cpp`(設定檔錯誤對話框)帶進 4 個新字「刪它將讓」,`App/fonts` 未重新產生(core 未改 App,見 w2-062 報告) |

**沒有驗證到**(只有一台已裝好所有工具的參考機):

- Qt Online Installer、Visual Studio Installer、Python 安裝程式的畫面與勾選項目名稱(依參考機已安裝的元件清單與
  安裝程式常見名稱撰寫,未實際重裝);Qt 命令列安裝。
- VS 2022(參考機只有 VS 18)與 Build Tools 版;沒有 `mingw_64` 的電腦上 qt-cmake 的行為(參考機有 mingw_64)。
- 「真的沒有 `C:\Qt`」的電腦:Qt 與 VS 是以 junction 模擬的位置,背後仍是同一份安裝;若有地方寫死預設路徑而恰好又存在,
  模擬無法完全排除(本輪已檢查模擬建置的 `CMakeCache.txt`:除了 CMake 自己的執行檔路徑 `CMAKE_COMMAND` 等被 Windows 解析成
  junction 的真實位置外,沒有 `C:/Qt/`、`C:/tools/emsdk`、`Visual Studio/18`)。emsdk 則是真的另外安裝一份。
- 缺少某個 Qt 模組時的實際錯誤訊息(§9 的訊息依 CMake / Qt 的一般行為撰寫)。
- 路徑含空白或中文的 repo / emsdk 位置;proxy / 防火牆環境下的 emsdk 下載;Qt Creator 的操作(§6)。
