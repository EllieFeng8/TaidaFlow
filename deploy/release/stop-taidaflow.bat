@echo off
rem TaidaFlow - FIELD stop by double-click (w2-057): nginx first, then the app window is closed
rem normally (like closing it by hand). Uses the same data folder as start-taidaflow.bat.
setlocal
set "DATADIR=%~dp0runtime"
if exist "%~dp0taidaflow-site.bat" call "%~dp0taidaflow-site.bat"
if "%DATADIR:~-1%"=="\" set "DATADIR=%DATADIR:~0,-1%"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop-taidaflow.ps1" -DataDir "%DATADIR%" %*
set "RC=%ERRORLEVEL%"
echo.
echo stop-taidaflow exit code %RC%
echo   0 = stopped   1 = nothing started by start-taidaflow was running
echo   7 = the app did not close in time (NOT killed - close the window by hand)   9 = nginx could not be stopped
if not defined TAIDAFLOW_NOPAUSE pause
exit /b %RC%
