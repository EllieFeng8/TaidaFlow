@echo off
rem w2-063 D2-01: docs\BUILD.md 3.3 "desktop manual build without preset", typed commands in a cmd window.
rem Output folder build\manual-desktop (does not touch build\desktop). Prints the exit code of every step.
rem Exit 0 only when every step returned 0.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
echo [w2-063] step vcvars64 exit=%ERRORLEVEL%
if errorlevel 1 exit /b 11
cd /d "%~dp0..\..\..\.."
echo [w2-063] cwd=%CD%
set VSLANG=1033
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build\manual-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/qt.toolchain.cmake
echo [w2-063] step configure exit=%ERRORLEVEL%
if errorlevel 1 exit /b 12
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\manual-desktop --target TaidaFlowApp
echo [w2-063] step build exit=%ERRORLEVEL%
if errorlevel 1 exit /b 13
dir build\manual-desktop\TaidaFlowApp.exe | findstr /i "TaidaFlowApp.exe"
exit /b 0
