@echo off
rem w2-077 D5: the CLion "Debug-Visual Studio" CMake profile on the command line (docs\BUILD.md 6A.3 A):
rem   toolchain "Visual Studio" = vcvars64 environment; CLion's bundled cmake (4.3.1) and ninja (1.13.2);
rem   options -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64, Build type Debug,
rem   build folder cmake-build-debug-visual-studio (CLion's default for that profile, git-ignored).
rem Usage: clion-debug.bat configure [fresh]   |   clion-debug.bat build [clean]
rem   CLION = CLion installation folder, default %LOCALAPPDATA%\Programs\CLion.  Exit code = cmake's.
setlocal
if not defined CLION set "CLION=%LOCALAPPDATA%\Programs\CLion"
set "CMAKE=%CLION%\bin\cmake\win\x64\bin\cmake.exe"
set "NINJA=%CLION%\bin\ninja\win\x64\ninja.exe"
for %%I in ("%~dp0..\..\..\..") do set "SRC=%%~fI"
if not exist "%CMAKE%" (echo [clion-debug] not found: %CMAKE% - set CLION & exit /b 2)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
echo [clion-debug] vcvars64 exit=%ERRORLEVEL%
cd /d "%SRC%" || exit /b 2
"%CMAKE%" --version | findstr /b "cmake version"
if /i "%~1"=="build" goto build
set "FRESH="
if /i "%~2"=="fresh" set "FRESH=--fresh"
echo [clion-debug] %DATE% %TIME% configure start (%SRC%)
"%CMAKE%" %FRESH% -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 "-DCMAKE_MAKE_PROGRAM=%NINJA:\=/%" -S . -B cmake-build-debug-visual-studio
set RC=%ERRORLEVEL%
echo [clion-debug] %DATE% %TIME% configure exit=%RC%
exit /b %RC%
:build
set "CLEAN="
if /i "%~2"=="clean" set "CLEAN=--clean-first"
echo [clion-debug] %DATE% %TIME% build start %CLEAN%
"%CMAKE%" --build cmake-build-debug-visual-studio %CLEAN%
set RC=%ERRORLEVEL%
echo [clion-debug] %DATE% %TIME% build exit=%RC%
exit /b %RC%
