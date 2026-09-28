@echo off
rem w2-063 D2-02: docs\BUILD.md 4.3 "WebAssembly manual build without preset (qt-cmake)", typed commands
rem in a NEW cmd window (no vcvars). Output folder build\manual-wasm (does not touch build\wasm-release).
rem Exit 0 only when every step returned 0.
call C:\tools\emsdk\emsdk_env.bat
echo [w2-063] step emsdk_env exit=%ERRORLEVEL%
if errorlevel 1 exit /b 21
call emcc --version
echo [w2-063] step emcc-version exit=%ERRORLEVEL%
cd /d "%~dp0..\..\..\.."
echo [w2-063] cwd=%CD%
call C:\Qt\6.8.3\wasm_singlethread\bin\qt-cmake.bat -S . -B build\manual-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DQT_HOST_PATH=C:/Qt/6.8.3/msvc2022_64 -DQT_HOST_PATH_CMAKE_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake -DBUILD_TESTING=OFF
echo [w2-063] step qt-cmake-configure exit=%ERRORLEVEL%
if errorlevel 1 exit /b 22
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\manual-wasm --target TaidaFlowApp
echo [w2-063] step build exit=%ERRORLEVEL%
if errorlevel 1 exit /b 23
dir build\manual-wasm\TaidaFlowApp.html build\manual-wasm\TaidaFlowApp.js build\manual-wasm\TaidaFlowApp.wasm build\manual-wasm\qtloader.js build\manual-wasm\qtlogo.svg | findstr /i "TaidaFlowApp qtloader qtlogo"
exit /b 0
