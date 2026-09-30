@echo off
rem w2-077 D5 (F2): CLion's DEFAULT toolchain (bundled MinGW, not switched to Visual Studio) with the
rem "Debug-Visual Studio" profile options. No vcvars64; PATH gets CLion's MinGW bin; C/C++ compilers =
rem CLion's gcc / g++ (what CLion passes for that toolchain). Build folder cmake-build-mingw-probe
rem (git-ignored), deleted first. The configure step MUST fail with the root CMakeLists.txt message
rem "... supports MSVC only ... Toolchains ... Visual Studio ...".
rem Usage: clion-mingw-probe.bat     Output also in build\w2-077-mingw-configure.log.
rem Exit code = cmake's configure exit code (non-zero expected).
setlocal
if not defined CLION set "CLION=%LOCALAPPDATA%\Programs\CLion"
set "CMAKE=%CLION%\bin\cmake\win\x64\bin\cmake.exe"
set "NINJA=%CLION%\bin\ninja\win\x64\ninja.exe"
set "MINGW=%CLION%\bin\mingw\bin"
for %%I in ("%~dp0..\..\..\..") do set "SRC=%%~fI"
if not exist "%MINGW%\g++.exe" (echo [clion-mingw-probe] not found: %MINGW%\g++.exe - set CLION & exit /b 2)
set "PATH=%MINGW%;%PATH%"
cd /d "%SRC%" || exit /b 2
if exist cmake-build-mingw-probe rmdir /s /q cmake-build-mingw-probe
if not exist build mkdir build
echo [clion-mingw-probe] %DATE% %TIME% configure start
"%CMAKE%" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 "-DCMAKE_MAKE_PROGRAM=%NINJA:\=/%" "-DCMAKE_C_COMPILER=%MINGW:\=/%/gcc.exe" "-DCMAKE_CXX_COMPILER=%MINGW:\=/%/g++.exe" -S . -B cmake-build-mingw-probe > build\w2-077-mingw-configure.log 2>&1
set RC=%ERRORLEVEL%
type build\w2-077-mingw-configure.log
echo [clion-mingw-probe] %DATE% %TIME% configure exit=%RC%
exit /b %RC%
