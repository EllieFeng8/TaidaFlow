@echo off
rem TaidaFlow - FIELD start by double-click (w2-057, w2-062). CONNECTS TO THE REAL EQUIPMENT.
rem Runs start-taidaflow.ps1 in this folder (no PowerShell knowledge needed).
rem All settings (data folder, nginx on/off and port, device addresses, ports) are in config.json
rem next to TaidaFlowApp.exe. It is created with the default values at the first start (data folder
rem C:\TaidaFlowData, nginx on port 80) and never overwritten by an update. Edit it with Notepad.
rem Extra arguments are passed on to start-taidaflow.ps1 (e.g. -DataDir D:\Test for one start).
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-taidaflow.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
echo start-taidaflow exit code %RC%
echo   0 = started   4 = already running or a port is in use (nothing started)
echo   6 = the app did not come up (see the log)   8 = started WITHOUT nginx
echo   2 = package, folder or config.json problem (see the message above)
echo   settings: %~dp0config.json   log folder: ^<data folder of config.json^>\logs
if not defined TAIDAFLOW_NOPAUSE pause
exit /b %RC%
