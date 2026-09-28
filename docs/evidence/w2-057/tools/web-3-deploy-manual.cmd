@echo on
rem DEPLOY_AND_STARTUP.md 2.2 manual copy (literal except build\desktop -> build\w2-057-webcopy, so the live build\desktop\web is not touched)
cd /d D:\repo\codex\qmlTester\taidaflow
rmdir /s /q build\w2-057-webcopy\web
mkdir build\w2-057-webcopy\web
for %%f in (TaidaFlowApp.html TaidaFlowApp.js TaidaFlowApp.wasm qtloader.js qtlogo.svg) do copy build\wasm-release\%%f build\w2-057-webcopy\web\
dir /b build\w2-057-webcopy\web
exit /b 0
