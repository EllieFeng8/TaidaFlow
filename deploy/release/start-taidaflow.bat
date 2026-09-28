@echo off
rem TaidaFlow - FIELD start by double-click (w2-057). CONNECTS TO THE REAL EQUIPMENT.
rem Runs start-taidaflow.ps1 in this folder (no PowerShell knowledge needed).
rem Defaults: data folder = <this folder>\runtime, nginx on port 80 (http://<IP>/).
rem To change them, copy taidaflow-site.example.bat to taidaflow-site.bat and edit it
rem (taidaflow-site.bat is not part of the package, so updates never overwrite it).
setlocal
set "DATADIR=%~dp0runtime"
set "USE_NGINX=1"
set "NGINX_PORT=80"
if exist "%~dp0taidaflow-site.bat" call "%~dp0taidaflow-site.bat"
if "%DATADIR:~-1%"=="\" set "DATADIR=%DATADIR:~0,-1%"
set "NGINXARGS="
if "%USE_NGINX%"=="1" set "NGINXARGS=-UseNginx -Port %NGINX_PORT%"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-taidaflow.ps1" -DataDir "%DATADIR%" %NGINXARGS% %*
set "RC=%ERRORLEVEL%"
echo.
echo start-taidaflow exit code %RC%
echo   0 = started   4 = already running or a port is in use (nothing started)
echo   6 = the app did not come up (see the log)   8 = started WITHOUT nginx   2 = package or folder problem
echo   log folder: %DATADIR%\logs
if not defined TAIDAFLOW_NOPAUSE pause
exit /b %RC%
