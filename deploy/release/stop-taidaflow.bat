@echo off
rem TaidaFlow - FIELD stop by double-click (w2-057, w2-062, w2-065): the app window is closed
rem normally (like closing it by hand). The data folder comes from config.json next to TaidaFlowApp.exe
rem (the same file start-taidaflow.bat uses). Extra arguments are passed on to stop-taidaflow.ps1.
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop-taidaflow.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
echo stop-taidaflow exit code %RC%
echo   0 = stopped   1 = nothing started by start-taidaflow was running
echo   7 = the app did not close in time (NOT killed - close the window by hand)
echo   2 = config.json problem (see the message above)
echo   nginx keeps running. To stop it too:  cd /d "%~dp0nginx"  then  nginx -s quit
echo   log: launcher-YYYY-MM-DD.log in the log folder (config.json log.dir, default C:\TaidaFlowData\logs)
if not defined TAIDAFLOW_NOPAUSE pause
exit /b %RC%
