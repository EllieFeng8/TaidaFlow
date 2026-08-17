# Wasm Mirror Integration Pack

這個目錄是可搬到既有 Qt/QML 專案的自包含 integration pack。它把大量同步行為
藏在一個深 Module 內，導入方只需要學會兩個主要 Interface：

```cpp
WasmMirrorConfig config;
auto mirror = WasmMirrorProxy::create(config, &error);
```

Module 內部已包含：

- Desktop WebSocket Server、WASM WebSocket Client。
- 單一 port、多個具名 Proxy。
- Q_PROPERTY Snapshot、Patch 與雙向寫入。
- 一般 typed signal 的 WASM → Desktop relay。
- contract hash、revision、session、防重播與防循環寫入。
- 斷線偵測、完整 Snapshot 後才 ready、自動退避重連。
- required／optional Proxy 與 per-proxy transport callback。

## 1. 直接複製什麼

把整個 `wasm-mirror` 發佈內容複製到既有專案，例如：

```text
MyExistingProject/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ src/
└─ third_party/
   └─ wasm-mirror/       ← 本 pack 完整目錄
```

不要只挑一個 `.cpp`。`src/`、`cmake/` 與 CMake relay generator 是同一個
Module 的 implementation，版本必須一致。

發布或直接複製資料夾時，必須排除所有 `build-*`、`CMakeCache.txt` 與編譯
產物；它們只屬於建立 pack 時的自測，不在 `MANIFEST.sha256` 內，也可能包含
建立者電腦的絕對路徑。Git 的 `.gitignore` 已排除它們，但製作 ZIP 時仍須檢查。

## 2. 目錄內容

```text
wasm-mirror/
├─ CMakeLists.txt                     可 add_subdirectory() 的 library target
├─ wasm-mirror-package-integration.md 既有專案的 package 導入手冊
├─ VERSION                            pack、wire 與來源版本
├─ MANIFEST.sha256                    核心檔案校驗值
├─ cmake/
│  ├─ WasmMirrorProxyRegister.cmake   Proxy class 註冊 Interface
│  └─ GenerateProxyRequestRelay.cmake moc JSON → typed relay implementation
├─ src/infrastructure/proxy_mirror/   Mirror 核心 C++ implementation
├─ examples/consumer/                 可獨立 configure/build 的最小 consumer
├─ templates/                         CLion/CMake 導入範本
├─ tests/                             pack 自身的 native contract/runtime tests
└─ docs/
   ├─ wasm-mirror-integration.md      完整導入指南
   └─ http-server-requirements.md     導入專案的 HTTP 責任
```

## 3. 必要工具

- CMake 3.21 以上。
- C++17。
- Qt 6.8.x：`Core`、`Network`、`WebSockets`。
- QML 專案另外需要 `Gui`、`Qml`、`Quick`、`QuickControls2`。
- Windows Desktop：Visual Studio/MSVC Qt kit。
- Qt 6.8.3 WASM：`wasm_singlethread` 與 Emscripten 3.1.56。
- 本團隊 EMSDK 標準位置：`C:/tools/emsdk`。

Mirror 不依賴 Modbus、OPC UA、SerialBus、React、Spring Boot、WebGateway 或
`QQmlApplicationEngine`。

## 4. 導入 CMake

在既有專案加入 pack：

```cmake
set(WASM_MIRROR_PACK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm-mirror")

add_subdirectory(
    "${WASM_MIRROR_PACK_DIR}"
    "${CMAKE_BINARY_DIR}/wasm-mirror-pack"
)

target_link_libraries(MyApplicationCore
    PUBLIC
        WasmMirror::Core
)
```

上例假設 `MyApplicationCore` 與 final executable 分離，而且 `main.cpp` 會使用
Mirror headers，所以需要 `PUBLIC` 傳遞 include/link usage requirements。若 Proxy、
`main.cpp` 與 Mirror 全在同一 executable target，該 executable 可改用 `PRIVATE`。

建立既有 target、設定 include directories/compile definitions 後，再註冊每一種
會加入 Config 的 concrete Proxy class：

```cmake
include("${WASM_MIRROR_PACK_DIR}/cmake/WasmMirrorProxyRegister.cmake")

wasm_mirror_register_proxy(
    TARGET MyApplicationCore
    CLASS OpcUaKepwareProxy
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/opcuakepwareproxy.h"
)

wasm_mirror_register_proxy(
    TARGET MyApplicationCore
    CLASS AlarmProxy
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/alarmproxy.h"
)

wasm_mirror_finalize_proxy_registration(TARGET MyApplicationCore)
```

規則：

1. `register` 必須在 target 建立後呼叫。
2. 同一 concrete class 只註冊一次；同類別的多個 instance 不重複註冊。
3. 所有類別完成後只呼叫一次 `finalize`。
4. `finalize` 後不可再註冊。
5. 至少 WASM 真正連結的 target 必須產生 relay。
6. 包含 Proxy `Q_OBJECT` 的 target 必須啟用 `AUTOMOC`；若專案沒有全域
   `CMAKE_AUTOMOC ON`，請加上：

```cmake
set_target_properties(MyApplicationCore PROPERTIES AUTOMOC ON)
```

完整 Desktop/WASM Qt module、tests 與 `WIN32_EXECUTABLE` 條件範例見
[`docs/wasm-mirror-integration.md`](docs/wasm-mirror-integration.md#11-desktop-與-wasm-的-cmake-條件)。

## 5. Proxy contract

會同步的 Q_PROPERTY 必須同時具備 `READ`、`WRITE`、`NOTIFY`：

```cpp
Q_PROPERTY(int testCount
           READ testCount
           WRITE setTestCount
           NOTIFY testCountChanged)
```

setter 必須真的寫入 member，並只在值改變時發出 NOTIFY：

```cpp
void MyProxy::setTestCount(int value)
{
    if (m_testCount == value) {
        return;
    }
    m_testCount = value;
    emit testCountChanged(m_testCount);
}
```

只供目前 target 顯示的本機狀態要排除同步：

```cpp
Q_PROPERTY(bool transportReady
           READ transportReady
           NOTIFY transportReadyChanged
           STORED false)
```

非 Q_PROPERTY NOTIFY 的 public signal 會視為 WASM UI event，不要求 `request`
前綴。Desktop Core 繼續使用原本的 `QObject::connect()`；沒有 handler 只代表功能
停用，不是 Mirror 啟動錯誤。

## 6. 在 main 建立 Config

QML 註冊與 Config 必須使用同一個 QObject instance：

```cpp
OpcUaKepwareProxy *opcua = obtainExistingProxy();

qmlRegisterSingletonInstance<OpcUaKepwareProxy>(
    "Core", 1, 0, "OpcuaKepware", opcua);

WasmMirrorConfig config;
config.host = QStringLiteral("127.0.0.1");
config.port = 8125;
config.path = QStringLiteral("/mirror");
config.allowedOrigins = {
    QStringLiteral("http://127.0.0.1:8123"),
    QStringLiteral("http://localhost:8123")
};

WasmMirrorProxyOptions options;
options.required = true;

#if defined(Q_OS_WASM)
opcua->setTransportState(
    false, QStringLiteral("Connecting to the Qt desktop Core..."));
options.transportStateHandler =
    [opcua](bool ready, const QString &message) {
        opcua->setTransportState(ready, message);
    };
#endif

if (!config.addProxy(QStringLiteral("OpcuaKepware"),
                     *opcua,
                     std::move(options))) {
    qCritical().noquote() << config.validationError();
    return EXIT_FAILURE;
}

QString error;
auto mirror = WasmMirrorProxy::create(config, &error);
if (!mirror) {
    qCritical().noquote() << error;
    return EXIT_FAILURE;
}

return app.exec(); // mirror 必須活到 event loop 結束
```

transport overlay 的 member 預設應為 `true`，讓沒有遠端 transport 的 Desktop UI
正常可操作。只有 WASM composition root 在載入 QML 前明確設成 `false`；後續再由
`transportStateHandler` 更新。不要把共用 Proxy member 無條件預設成 `false`。

多個 Proxy 全部加入同一個 Config，共用一個 port：

```cpp
config.addProxy(QStringLiteral("OpcuaKepware"), *opcua);
config.addProxy(QStringLiteral("Alarm"), *alarm);
config.addProxy(QStringLiteral("Settings"), *settings);
```

## 7. Desktop 與 WASM 的責任

| 責任 | Desktop | WASM |
|---|---:|---:|
| authoritative hardware Core | 建立 | 不建立 |
| QML-facing Proxy | 建立 | 建立 replica |
| pure local logic Core | 可建立 | 可各自建立 |
| Mirror | WebSocket Server | WebSocket Client |
| HTTP static server | 導入專案提供 | 由瀏覽器使用 |

若 Proxy 建構子會啟動硬體/thread，才增加 `MirrorReplica` mode；被動 Proxy 不需要
改建構方式。mode 只能關閉副作用，不可讓 Desktop/WASM 出現不同 Q_PROPERTY 或
signal contract。

## 8. HTTP server 不在 pack 內

Mirror 只提供 WebSocket。導入專案必須自行用 HTTP(S) 提供 Qt 產生的：

```text
MyApplication.html
MyApplication.js
MyApplication.wasm
qtloader.js
```

不可用 `file://`。頁面的完整 Origin 必須加入 `allowedOrigins`。詳細要求見
[`docs/http-server-requirements.md`](docs/http-server-requirements.md)。

## 9. Build 與驗證

本 pack 不依賴 `.cmd`／`.ps1`。從 CLion 選既有專案的 CMake preset：

- Desktop：MSVC Desktop configure/build preset。
- WASM：Qt `wasm_singlethread` configure preset，以及繼承 configure environment
  的 WASM build preset。

要先驗證 pack 自身，可把 pack 當 top-level project configure，設定：

```text
WASM_MIRROR_BUILD_TESTS=ON
```

再執行 CTest，應看到：

```text
WasmMirror.Engine
WasmMirror.Runtime
```

`examples/consumer` 是可獨立 configure/build 的最小 consumer；它刻意不提供
HTTP server，也不包含產品硬體 Core。

本版 pack 已獨立通過 native tests `2/2`、MSVC consumer build 與 Qt WASM
consumer build。內容雜湊與驗證基準記錄於 `MANIFEST.sha256`、`PROVENANCE.md`。

## 10. 導入檢查表

- [ ] 整個 pack 已放入同一版本的目錄。
- [ ] Host target 已連結 `WasmMirror::Core`。
- [ ] 每種 concrete Proxy class 已 register，最後已 finalize。
- [ ] Desktop/WASM 使用同一份 Proxy header。
- [ ] 所有同步 property 都有 READ／WRITE／NOTIFY。
- [ ] 本機狀態使用 `STORED false` 或 `CONSTANT`。
- [ ] QML 與 Config 使用同一 QObject instance。
- [ ] authoritative hardware Core 只在 Desktop 建立。
- [ ] Proxy 與 Mirror runtime 都在 GUI/main thread，且 Proxy 比 runtime 活得久。
- [ ] Desktop/WASM 使用相同固定 port、path 與 mirrorName。
- [ ] HTTP server 指向最新 WASM 產物，Origin 已加入 allowlist。
- [ ] Desktop tests、WASM build 與真實 browser reconnect 測試已通過。

完整規格、故障排查與多 Proxy 語意請閱讀
[`docs/wasm-mirror-integration.md`](docs/wasm-mirror-integration.md)。

要把本 package 接入另一個既有專案，請從
[`wasm-mirror-package-integration.md`](wasm-mirror-package-integration.md) 開始。
