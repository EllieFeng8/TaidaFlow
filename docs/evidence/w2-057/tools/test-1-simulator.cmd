@echo on
rem DEPLOY_AND_STARTUP.md 3.2 header + step 1 (literal)
set "REPO=D:\repo\codex\qmlTester"
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
mkdir "%REPO%\taidaflow\build\sim-cwd" 2>nul
cd /d "%REPO%\taidaflow\build\sim-cwd"
start "" "%REPO%\Adam60xxSimulator\build\Adam60xxSimulator.exe" --autostart
ping -n 4 127.0.0.1 >nul
netstat -ano | findstr LISTENING | findstr ":502 "
exit /b 0
