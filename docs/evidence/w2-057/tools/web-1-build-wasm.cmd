@echo on
rem DEPLOY_AND_STARTUP.md 2.1 (script form)
cd /d D:\repo\codex\qmlTester\taidaflow
call scripts\build-wasm.bat
exit /b %ERRORLEVEL%
