@echo off
rem TaidaFlow - FIELD start by double-click (w2-057, w2-060). CONNECTS TO THE REAL EQUIPMENT.
rem Runs start-taidaflow.ps1 in this folder (no PowerShell knowledge needed).
rem All site settings come from config.json in this folder (dataDir, useNginx, nginxPort,
rem restPort - see DEPLOY.md section 1.2); there is no other site configuration file.
rem Extra parameters are passed on to start-taidaflow.ps1 for this one run,
rem e.g.  start-taidaflow.bat -Port 8123
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-taidaflow.ps1" %*
set "RC=%ERRORLEVEL%"
rem Data folder for the message below: dataDir of config.json (read with the same reader as the script).
set "DATADIR=%~dp0runtime"
for /f "usebackq delims=" %%D in (`powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ". '%~dp0scripts\taidaflow-config.ps1'; $c = Read-TaidaFlowConfig '%~dp0.'; if ($c.DataDir) { $c.DataDir }"`) do set "DATADIR=%%D"
echo.
echo start-taidaflow exit code %RC%
echo   0 = started   4 = already running or a port is in use (nothing started)
echo   6 = the app did not come up (see the log)   8 = started WITHOUT nginx
echo   2 = package, folder or config.json problem (see the lines above)
echo   data folder (config.json dataDir): %DATADIR%   log folder: %DATADIR%\logs
if not defined TAIDAFLOW_NOPAUSE pause
exit /b %RC%
