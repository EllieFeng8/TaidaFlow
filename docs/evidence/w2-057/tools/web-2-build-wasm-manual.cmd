@echo on
rem DEPLOY_AND_STARTUP.md 2.1 (manual equivalent, literal)
call C:\tools\emsdk\emsdk_env.bat
cd /d D:\repo\codex\qmlTester\taidaflow
C:\Qt\Tools\CMake_64\bin\cmake.exe --preset wasm-release
if errorlevel 1 exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build --preset wasm-release
exit /b %ERRORLEVEL%
