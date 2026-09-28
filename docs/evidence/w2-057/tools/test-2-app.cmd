@echo on
rem DEPLOY_AND_STARTUP.md 3.2 header + step 2 with the "keep a log" variant (set QT_FORCE_STDERR_LOGGING=1 + start /b cmd /c ... 2> manual.log)
set "REPO=D:\repo\codex\qmlTester"
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
set TAIDAFLOW_DEVICE_PROFILE=simulator
set TAIDAFLOW_DOWNLOAD_PORT=80
mkdir "%REPO%\taidaflow\build\runtime-cwd" 2>nul
cd /d "%REPO%\taidaflow\build\runtime-cwd"
set QT_FORCE_STDERR_LOGGING=1
start "" /b cmd /c ""%REPO%\taidaflow\build\desktop\TaidaFlowApp.exe" 2> "%REPO%\taidaflow\build\runtime-logs\manual.log""
ping -n 8 127.0.0.1 >nul
netstat -ano | findstr LISTENING | findstr ":8124 :8125 :18125"
exit /b 0
