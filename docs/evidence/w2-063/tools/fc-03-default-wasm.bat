@echo off
rem w2-063 fresh-clone step 3 (default tool paths): in build\fc
rem  1) scripts\build-wasm.bat             (BUILD.md 5.1; full build, preset mode)
rem  2) BUILD.md 5.2 manual A (emsdk_env + cmake --preset wasm-release + cmake --build --preset)
rem Exit 0 only when every step returned 0.
set TAIDAFLOW_QT_ROOT=
set TAIDAFLOW_QT_TOOLS=
set TAIDAFLOW_VCVARS64=
set TAIDAFLOW_EMSDK=
set TAIDAFLOW_NO_PRESET=
cd /d "%~dp0..\..\..\..\build\fc" || exit /b 1
echo [w2-063] cwd=%CD%
call scripts\build-wasm.bat
echo [w2-063] step build-wasm.bat exit=%ERRORLEVEL%
if errorlevel 1 exit /b 31
cmd /c "call C:\tools\emsdk\emsdk_env.bat && C:\Qt\Tools\CMake_64\bin\cmake.exe --preset wasm-release && C:\Qt\Tools\CMake_64\bin\cmake.exe --build --preset wasm-release"
echo [w2-063] step manual-A emsdk_env+preset+build exit=%ERRORLEVEL%
if errorlevel 1 exit /b 32
dir build\wasm-release\TaidaFlowApp.html build\wasm-release\TaidaFlowApp.js build\wasm-release\TaidaFlowApp.wasm build\wasm-release\qtloader.js build\wasm-release\qtlogo.svg | findstr /i "TaidaFlowApp qtloader qtlogo"
exit /b 0
