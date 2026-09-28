@echo on
rem DEPLOY_AND_STARTUP.md 1.4 (c) lines 1-3 (-t, start /min, curl -I); C:/TaidaFlowData -> the test data folder.
set "TF=D:\repo\codex\qmlTester\taidaflow\dist\TaidaFlow-20260928-c714aee"
"%TF%\nginx\nginx.exe" -p "D:/repo/codex/qmlTester/taidaflow/build/w2-057-manual-prod/nginx/" -c conf/taidaflow.conf -t
echo nginx -t exit %ERRORLEVEL%
start "TaidaFlow nginx" /min "%TF%\nginx\nginx.exe" -p "D:/repo/codex/qmlTester/taidaflow/build/w2-057-manual-prod/nginx/" -c conf/taidaflow.conf
ping -n 3 127.0.0.1 >nul
curl -I http://127.0.0.1/
echo curl exit %ERRORLEVEL%
exit /b 0
