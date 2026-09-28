@echo on
rem DEPLOY_AND_STARTUP.md 3.2 step 3: mkdir + copy (literal); the Notepad replace is done by the harness.
set "REPO=D:\repo\codex\qmlTester"
mkdir "%REPO%\taidaflow\build\nginx-manual\conf" "%REPO%\taidaflow\build\nginx-manual\logs" "%REPO%\taidaflow\build\nginx-manual\temp"
copy "%REPO%\taidaflow\deploy\nginx\taidaflow.conf" "%REPO%\taidaflow\build\nginx-manual\conf\taidaflow.conf"
exit /b %ERRORLEVEL%
