@echo off
rem w2-077 D5: the static checks after the CLion builds (no program is started).
rem   [1] make-font-subset --check
rem   [2] check-wasm-backend on the CLion Debug folder (CLion's ninja, -Ninja)
rem   [3] check-wasm-backend on the Debug folder with Qt's ninja 1.12.1 (default)
rem   [4] check-wasm-backend on the preset Release + wasm-release folders
rem   [5] check-version-shadow on the Debug folder (CLion's ninja) and on Release + wasm-release
rem   [6] check-rest-routes      [7] verify-pack
rem   [8] test-check-wasm-backend.ps1 (positive / old-vs-new / mutation tests of F1)
rem Usage: run-checks.bat     Exit code 0 = every step exit 0.
setlocal
if not defined CLION set "CLION=%LOCALAPPDATA%\Programs\CLion"
set "CNINJA=%CLION%\bin\ninja\win\x64\ninja.exe"
for %%I in ("%~dp0..\..\..\..") do set "SRC=%%~fI"
cd /d "%SRC%" || exit /b 2
set "PS=powershell -NoProfile -ExecutionPolicy Bypass -File"
set "DBG=cmake-build-debug-visual-studio"
set "REL=build\w2-077-desktop-release"
set "WASM=build\w2-077-wasm-release"
set FAILED=0

echo ===== [1] make-font-subset --check
%PS% scripts\make-font-subset.ps1 --check
call :rc 1 %ERRORLEVEL%
echo ===== [2] check-wasm-backend -Ninja CLion %DBG%
%PS% scripts\check-wasm-backend.ps1 -Ninja "%CNINJA%" %DBG%
call :rc 2 %ERRORLEVEL%
echo ===== [3] check-wasm-backend (Qt ninja) %DBG% %WASM%
%PS% scripts\check-wasm-backend.ps1 %DBG% %WASM%
call :rc 3 %ERRORLEVEL%
echo ===== [4] check-wasm-backend %REL% %WASM%
%PS% scripts\check-wasm-backend.ps1 %REL% %WASM%
call :rc 4 %ERRORLEVEL%
echo ===== [5a] check-version-shadow -Ninja CLion %DBG%
%PS% scripts\check-version-shadow.ps1 -Ninja "%CNINJA%" %DBG%
call :rc 5a %ERRORLEVEL%
echo ===== [5b] check-version-shadow %REL% %WASM%
%PS% scripts\check-version-shadow.ps1 %REL% %WASM%
call :rc 5b %ERRORLEVEL%
echo ===== [6] check-rest-routes
%PS% scripts\check-rest-routes.ps1
call :rc 6 %ERRORLEVEL%
echo ===== [7] verify-pack
%PS% scripts\verify-pack.ps1
call :rc 7 %ERRORLEVEL%
echo ===== [8] test-check-wasm-backend
%PS% docs\evidence\w2-077\tools\test-check-wasm-backend.ps1 -DebugDir %DBG% -ReleaseDir %REL% -WasmDir %WASM%
call :rc 8 %ERRORLEVEL%

echo ===== run-checks failed steps: %FAILED%
if "%FAILED%"=="0" exit /b 0
exit /b 1

:rc
echo ----- [%1] exit=%2
if not "%2"=="0" set /a FAILED+=1
exit /b 0
