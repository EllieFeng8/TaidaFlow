# AppHttpServer

可重用的 HTTP 伺服器**單例**(Qt 6.8,`QHttpServer`)。一個程序一個實例,跑在自己的執行緒,
提供靜態網頁(例如 Qt for WebAssembly 的 build)、下載資料夾與自訂 GET 路由。
只依賴 Qt **Core / Network / HttpServer**,不認識使用它的應用程式:像插件一樣,把這個資料夾的
兩個檔案放進任何 Qt 6.8 專案就能用。

在 TaidaFlow(w2-049)裡,它取代了舊的開發用網頁伺服器(8123)與 `HistoryExport` 內的
下載伺服器:desktop 在 `0.0.0.0:8124`(w2-062 起為 config.json `http.bind`/`http.port`)同時提供網頁 `/`
與 CSV 下載 `/exports/<檔名>`。

## 功能

| API | 說明 |
|---|---|
| `AppHttpServer::instance()` | 取得單例(第一次呼叫時建立,程序結束時 `stop()`) |
| `start(port, address = AnyIPv4)` | 在伺服器執行緒上 bind;已在同位址/port 聽 → `true`;port 0 = 任意空 port(`port()` 取得)。失敗記 warning、`lastError()`、回 `false`,**不結束程式**,之後可再呼叫 |
| `stop()` / `isListening()` / `port()` / `address()` / `lastError()` | 關閉 listener 與所有連線、停止執行緒;狀態查詢 |
| `mountStatic(prefix, dir, StaticOptions)` | 靜態檔(見下) |
| `mountDownloads(prefix, dir, validator, DownloadOptions)` | 單層下載資料夾;`validator(fileName)` 決定哪些檔名可下載 |
| `addGetRoute(path, handler)` | 自訂 GET 路由(完全相符的路徑),`handler(const QHttpServerRequest&, QHttpServerResponder&)` 在伺服器執行緒被呼叫 |
| `unmount(prefix)` / `removeGetRoute(path)` / `mountedPrefixes()` / `routePaths()` | 取消掛載 / 查詢 |

- **執行緒安全**:所有註冊函式與 `start()`/`stop()` 可在任何執行緒、`start()` 前後任意呼叫;
  掛載與路由在 `stop()`/`start()` 之間保留。比對順序:自訂路由(完全相符)→ 最長的掛載前綴。
- **一律串流**:檔案以 `QHttpServerResponder::write(QIODevice*)` 從磁碟分段送出,不整檔讀進記憶體
  (QTest 量測:1 GiB 下載,程序 private memory 增加約 4 MB;讀取端停 3 秒時也不增加)。
- 只接受 `GET` / `HEAD`(自訂路由只接受 `GET`),其他方法回 405(`Allow`)。
- 不要在路由 handler 裡呼叫 `stop()`/`start()`(伺服器執行緒不能等自己)。

### 靜態掛載(`StaticOptions`)

- MIME:`.html/.htm` `text/html; charset=utf-8`、`.js/.mjs` `text/javascript; charset=utf-8`、
  `.wasm` `application/wasm`、`.css`、`.json`、`.map`、`.txt`、`.csv`、`.xml`、`.png`、`.jpg`、`.gif`、
  `.webp`、`.svg`、`.ico`、`.ttf`、`.otf`、`.woff`、`.woff2`、`.pdf`、`.gz`、`.zip`;其他
  `application/octet-stream`;`extraMimeTypes` 可增補/覆寫。
- 快取:`ETag`(檔案大小 + 修改時間)、`Last-Modified`;`If-None-Match`(優先)/ `If-Modified-Since`
  相符 → 304。`htmlCacheControl` / `cacheControl` 預設都是 `no-cache`(瀏覽器每次重新驗證),
  重新建置後 ETag 改變 → 200 新檔,不會新舊混用。
  `fileCacheControl`(w2-062):個別檔名(小寫、不含資料夾)→ 自己的 `Cache-Control`,優先於上面兩個值;
  TaidaFlow 用 `{"runtime.json": "no-store"}`(程式每次啟動都會重寫這個檔,瀏覽器不可快取)。
  該檔其餘規則(MIME、ETag、.gz)不變。
- gzip(`gzipVariants`,預設開):有 `<檔名>.gz`、**不比原檔舊**、請求的 `Accept-Encoding` 接受 gzip →
  送 `.gz`(`Content-Encoding: gzip`、MIME 仍為原檔、ETag 另帶 `-gz`);回應都帶 `Vary: Accept-Encoding`。
- `crossOriginIsolation`(預設開):`Cross-Origin-Opener-Policy: same-origin` +
  `Cross-Origin-Embedder-Policy: require-corp`;`crossOriginResourcePolicy`(預設開):
  `Cross-Origin-Resource-Policy: same-origin`。
- 目錄:`indexFile` 有設定且存在 → `redirectToIndex` 為 true 時 302 到 `<目錄>/<indexFile>`,否則直接送;
  沒有 → 404。**不列目錄**。
- `fileSuffixes`:只送這些副檔名(空 = 全部),掛載 build 資料夾時可避免送出 CMake/Ninja 檔。
- `allowHiddenFiles`(預設關):`.` 開頭的路徑段一律 404。
- 路徑穿越一律 400/403/404:`..`、`.`、`%2e%2e`、編碼過的 `/`(`%2f`)與 `\`(`%5c`)、反斜線、
  空路徑段(`//`)、`:`(磁碟代號、NTFS stream)、萬用字元與控制字元、結尾 `.`/空白、Windows 裝置名
  (`NUL`、`CON`、`COM1` …),以及掛載資料夾**底下**的符號連結 / junction / 捷徑(一律不跟隨;
  掛載資料夾本身可以是連結)。實際路徑另以 `canonicalFilePath()` 檢查必須在資料夾內。

### 下載掛載(`DownloadOptions`)

- 只接受前綴下**一層**檔名,由呼叫端的 `validator` 決定(例如 `^[A-Za-z0-9_-]{1,40}_\d{8}_\d{6}\.csv$`);
  不合 → 400;不存在 → 404;符號連結或實際路徑不在資料夾內 → 403。
- 回應:MIME 依副檔名、`Content-Disposition: attachment; filename="<檔名>"`(非 ASCII 另帶
  `filename*=UTF-8''...`)、`Access-Control-Allow-Origin: *`、`Cache-Control: no-store`、`Content-Length`。

## 放進其他 Qt 6.8 專案

1. 複製 `AppHttpServer.h`、`AppHttpServer.cpp`(本資料夾;`tests/` 可一起帶走)。
2. CMake(不需要 AUTOMOC:類別沒有 `Q_OBJECT`):

```cmake
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network HttpServer)
target_sources(myapp PRIVATE AppHttpServer/AppHttpServer.h AppHttpServer/AppHttpServer.cpp)
target_link_libraries(myapp PRIVATE Qt6::Core Qt6::Network Qt6::HttpServer)
```

3. 使用:

```cpp
#include "AppHttpServer/AppHttpServer.h"

AppHttpServer::StaticOptions web;
web.indexFile = QStringLiteral("index.html");            // "/" -> 302 /index.html
AppHttpServer::instance().mountStatic(QStringLiteral("/"), QStringLiteral("C:/myapp/web"), web);
AppHttpServer::instance().mountDownloads(QStringLiteral("/files"), QStringLiteral("C:/myapp/out"),
    [](const QString &name) { return name.endsWith(QLatin1String(".csv")); });
AppHttpServer::instance().addGetRoute(QStringLiteral("/api/ping"),
    [](const QHttpServerRequest &, QHttpServerResponder &r) { r.write("pong", "text/plain"); });
if (!AppHttpServer::instance().start(8124))              // 失敗只記 log,程式照常
    qWarning() << AppHttpServer::instance().lastError();
QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { AppHttpServer::instance().stop(); });
```

log 走 logging category `apphttpserver`(訊息前綴 `[AppHttpServer]`),可用
`QT_LOGGING_RULES="apphttpserver.info=false"` 關掉每個請求的紀錄。

## 測試(Qt Test,CTest)

`tests/` 是獨立的 CMake 專案:只編 `../AppHttpServer.{h,cpp}` 與 `tst_apphttpserver.cpp`,只連 Qt
(Core、Network、HttpServer、Test;Windows 另連 `psapi` 量記憶體),不連結任何應用程式原始碼。
每個請求都走真的 TCP 連線(原始 HTTP/1.1,不經客戶端路徑整理),port 由 OS 挑空的。

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
C:\Qt\Tools\CMake_64\bin\cmake.exe -S Core\AppHttpServer\tests -B build\apphttpserver-qtest -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\apphttpserver-qtest
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir build\apphttpserver-qtest -V
```

(TaidaFlow:`scripts\run-apphttpserver-tests.bat` 做同樣的事。)涵蓋:MIME、200/304(ETag、
Last-Modified、重建後 200)、gzip 變體(含 `q=0`、過期 .gz 不送)、HEAD、COOP/COEP/CORP、預設頁 /
不列目錄 / 副檔名白名單、穿越攻擊 27 種(含 junction)、下載掛載(白名單、子資料夾、符號連結、405)、
自訂路由(在伺服器執行緒、優先於掛載)、綁定失敗與重新啟動、9 條執行緒同時註冊時請求不出錯、
1 GiB 串流的記憶體(一般與讀取端停頓)。需要約 2 GiB 暫存磁碟空間(測完刪除)。

來源:TaidaFlow `Core/HistoryExport.cpp` 原本的 `ExportDownloadServer`(w2-041)改寫、擴充而成。
