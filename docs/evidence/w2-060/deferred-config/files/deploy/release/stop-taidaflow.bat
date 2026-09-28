@echo off
rem TaidaFlow - FIELD stop by double-click (w2-057, w2-060): nginx first, then the app window is
rem closed normally (like closing it by hand). The data folder comes from config.json in this
rem folder (the same file start-taidaflow.bat uses). Extra parameters are passed on for this run.
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop-taidaflow.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
echo stop-taidaflow exit code %RC%
echo   0 = stopped   1 = nothing started by start-taidaflow was running   2 = config.json problem
echo   7 = the app did not close in time (NOT killed - close the window by hand)   9 = nginx could not be stopped
if not defined TAIDAFLOW_NOPAUSE pause
exit /b %RC%
