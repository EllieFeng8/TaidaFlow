@echo off
rem w2-063 fresh-clone step 2 (default tool paths, i.e. behaviour unchanged): in build\fc
rem  1) scripts\build-desktop.bat          (BUILD.md 4.1; full build, preset mode)
rem  2) BUILD.md 4.2 manual A typed in a cmd window (vcvars64 + cmake --preset + cmake --build --preset)
rem Exit 0 only when every step returned 0.
set TAIDAFLOW_QT_ROOT=
set TAIDAFLOW_QT_TOOLS=
set TAIDAFLOW_VCVARS64=
set TAIDAFLOW_EMSDK=
set TAIDAFLOW_NO_PRESET=
cd /d "%~dp0..\..\..\..\build\fc" || exit /b 1
echo [w2-063] cwd=%CD%
call scripts\build-desktop.bat
echo [w2-063] step build-desktop.bat exit=%ERRORLEVEL%
if errorlevel 1 exit /b 21
cmd /c "call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && C:\Qt\Tools\CMake_64\bin\cmake.exe --preset desktop-release && C:\Qt\Tools\CMake_64\bin\cmake.exe --build --preset desktop-release"
echo [w2-063] step manual-A vcvars64+preset+build exit=%ERRORLEVEL%
if errorlevel 1 exit /b 22
dir build\desktop\TaidaFlowApp.exe | findstr /i "TaidaFlowApp.exe"
exit /b 0
