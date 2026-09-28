@echo on
rem DEPLOY_AND_STARTUP.md 3.2 step 3 (after the replace) + step 4 curl checks (literal)
set "NGX=C:\tools\nginx\nginx-1.30.5\nginx.exe"
"%NGX%" -p "D:/repo/codex/qmlTester/taidaflow/build/nginx-manual/" -c conf/taidaflow.conf -t
echo nginx -t exit %ERRORLEVEL%
start "TaidaFlow nginx" /min "%NGX%" -p "D:/repo/codex/qmlTester/taidaflow/build/nginx-manual/" -c conf/taidaflow.conf
ping -n 3 127.0.0.1 >nul
curl -I http://127.0.0.1/
curl -I http://127.0.0.1/TaidaFlowApp.html
curl -I http://127.0.0.1:8124/TaidaFlowApp.html
exit /b 0
