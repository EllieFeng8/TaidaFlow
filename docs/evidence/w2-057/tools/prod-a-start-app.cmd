@echo on
rem DEPLOY_AND_STARTUP.md 1.4 (a) - literal commands; TF / DATA set to the test package and a test data folder.
rem PATH is reduced to the Windows folders (the plant PC has no Qt at all).
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem"
set "TF=D:\repo\codex\qmlTester\taidaflow\dist\TaidaFlow-20260928-c714aee"
set "DATA=D:\repo\codex\qmlTester\taidaflow\build\w2-057-manual-prod"
mkdir "%DATA%" 2>nul
cd /d "%DATA%"
set TAIDAFLOW_DEVICE_PROFILE=
set TAIDAFLOW_E2E_PV_FILE=
set QT_PLUGIN_PATH=
set QML_IMPORT_PATH=
set QML2_IMPORT_PATH=
set TAIDAFLOW_DOWNLOAD_PORT=80
start "" "%TF%\TaidaFlowApp.exe"
exit /b %ERRORLEVEL%
