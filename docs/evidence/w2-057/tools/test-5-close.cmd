@echo on
rem DEPLOY_AND_STARTUP.md 3.2 step 5 (literal)
set "NGX=C:\tools\nginx\nginx-1.30.5\nginx.exe"
"%NGX%" -p "D:/repo/codex/qmlTester/taidaflow/build/nginx-manual/" -c conf/taidaflow.conf -s quit
echo nginx -s quit exit %ERRORLEVEL%
taskkill /IM TaidaFlowApp.exe
echo taskkill app exit %ERRORLEVEL%
ping -n 6 127.0.0.1 >nul
taskkill /IM Adam60xxSimulator.exe
echo taskkill simulator exit %ERRORLEVEL%
ping -n 4 127.0.0.1 >nul
netstat -ano | findstr LISTENING | findstr /C:":80 " /C:":502 " /C:":8124 " /C:":8125 " /C:":18125 "
echo findstr exit %ERRORLEVEL% (1 = nothing listening)
exit /b 0
