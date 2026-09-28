@echo off
rem w2-063 D2-02a: reproduce why BUILD.md 5.3 passes BOTH QT_HOST_PATH and QT_HOST_PATH_CMAKE_DIR.
rem Configure only (no build) with QT_HOST_PATH alone into build\manual-wasm-hostcheck, print the
rem resulting QT_HOST_PATH* cache entries, then delete the folder. Exit = configure exit code.
call C:\tools\emsdk\emsdk_env.bat >nul 2>&1
cd /d "%~dp0..\..\..\.."
call C:\Qt\6.8.3\wasm_singlethread\bin\qt-cmake.bat -S . -B build\manual-wasm-hostcheck -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DQT_HOST_PATH=C:/Qt/6.8.3/msvc2022_64 -DBUILD_TESTING=OFF >nul
set RC=%ERRORLEVEL%
echo [w2-063] configure (QT_HOST_PATH only) exit=%RC%
findstr /b "QT_HOST_PATH" build\manual-wasm-hostcheck\CMakeCache.txt
rmdir /s /q build\manual-wasm-hostcheck
exit /b %RC%
