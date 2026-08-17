# WASM HTTP Server 導入責任

`WasmMirrorProxy` 是 WebSocket 同步 Module，不是 HTTP static-file server。導入專案
必須自行提供或整合 HTTP server。

## 必要行為

1. 以 HTTP(S) 提供同一次 WASM build 產生的 HTML、JS、WASM 與 `qtloader.js`。
2. 正確 MIME：`.js` 使用 JavaScript MIME，`.wasm` 使用 `application/wasm`。
3. 開發期間避免舊快取讓 HTML、JS、WASM 版本混用。
4. 頁面 Origin 必須列入 `WasmMirrorConfig::allowedOrigins`。
5. 頁面內的 WebSocket URL 必須與 Config 的 host、port、path 一致。
6. HTTPS 頁面必須搭配 WSS；目前 pack 的 loopback 範例使用 `ws://`。

## 不可使用

- 不可直接用 `file://` 開啟 Qt WASM HTML。
- 不可讓 HTTP server 指向舊 build directory。
- 不可把 Mirror WebSocket port 當成 HTTP port。

## Port 範例

```text
HTTP static files : http://127.0.0.1:8123/
Mirror WebSocket  : ws://127.0.0.1:8125/mirror
```

HTTP server 可以是導入專案既有的 QHttpServer、產品 web server、反向代理或開發用
static server。它不是本 pack 的一部分。

