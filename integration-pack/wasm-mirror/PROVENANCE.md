# Pack provenance

- Pack version：`1.0.0`
- Assembled：`2026-08-17`
- Source code baseline commit：`d52bd18a56527a6b2ba36b06c1668519df8e656d`
- Wire protocol：`3`
- Inner ProxyMirror protocol：`2`
- Qt baseline：`6.8.x`
- C++ baseline：`C++17`

核心 C++、CMake generator 與測試由上述 commit 複製；整合指南則取自
`2026-08-17` 的最新工作目錄版本。每一個交付檔案的精確內容以
`MANIFEST.sha256` 為準。

本 pack 建立後已獨立驗證：

- Native MSVC pack tests：`2/2` 通過。
- Consumer example：MSVC Debug 編譯通過。
- Consumer example：Qt 6.8.3 `wasm_singlethread` 編譯通過，產生
  HTML／JavaScript／WASM／`qtloader.js`。

更新 pack 時必須一起更新全部核心檔案、重新產生 `MANIFEST.sha256`，並重新執行
native tests、WASM build 與真實 browser 測試。不要只替換其中一個 `.cpp`。
