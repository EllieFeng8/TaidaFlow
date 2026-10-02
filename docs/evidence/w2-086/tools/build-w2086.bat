@echo off
rem w2-086: the task's own builds - the same configuration as scripts\build-desktop.bat (preset
rem desktop-release) and scripts\build-wasm.bat wasm-release (preset wasm-release), but into
rem build\w2-086-desktop and build\w2-086-wasm. build\desktop and build\wasm-release (in use by Mango)
rem are never touched. The command lines are the "no preset" branches of those two scripts with
rem the default tool paths (C:\Qt\6.8.3, C:\Qt\Tools, C:\tools\emsdk).
rem Usage: build-w2086.bat desktop|wasm [fresh]      Exit code: 0 = success.
setlocal
set "TF=%~dp0..\..\..\.."
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
set "NINJA=C:/Qt/Tools/Ninja/ninja.exe"
pushd "%TF%" || exit /b 1
if /I "%~1"=="desktop" goto desktop
if /I "%~1"=="wasm" goto wasm
echo usage: build-w2086.bat desktop^|wasm [fresh]
popd
exit /b 2

:desktop
if /I "%~2"=="fresh" if exist "build\w2-086-desktop" rmdir /s /q "build\w2-086-desktop"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || (popd & exit /b 1)
set "VSLANG=1033"
"%CMAKE%" -S . -B build\w2-086-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA%" "-DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/qt.toolchain.cmake" || (popd & exit /b 1)
"%CMAKE%" --build build\w2-086-desktop --target TaidaFlowApp || (popd & exit /b 1)
popd
exit /b 0

:wasm
if /I "%~2"=="fresh" if exist "build\w2-086-wasm" rmdir /s /q "build\w2-086-wasm"
call "C:\tools\emsdk\emsdk_env.bat" >nul 2>&1
"%CMAKE%" -S . -B build\w2-086-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA%" "-DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.8.3/wasm_singlethread/lib/cmake/Qt6/qt.toolchain.cmake" "-DQT_CHAINLOAD_TOOLCHAIN_FILE=C:/tools/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" "-DEMSCRIPTEN_ROOT_PATH=C:/tools/emsdk/upstream/emscripten" "-DQT_HOST_PATH=C:/Qt/6.8.3/msvc2022_64" "-DQT_HOST_PATH_CMAKE_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake" "-DCMAKE_CROSSCOMPILING_EMULATOR=%EMSDK_NODE:\=/%" -DBUILD_TESTING=OFF || (popd & exit /b 1)
"%CMAKE%" --build build\w2-086-wasm --target TaidaFlowApp || (popd & exit /b 1)
popd
exit /b 0
