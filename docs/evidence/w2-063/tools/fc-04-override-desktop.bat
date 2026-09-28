@echo off
rem w2-063 fresh-clone step 4 (BUILD.md 2.6, tools in other locations, desktop): junctions under
rem build\fc-tools point to the real Qt and Visual Studio; TAIDAFLOW_QT_ROOT / TAIDAFLOW_QT_TOOLS /
rem TAIDAFLOW_VCVARS64 name the junction paths; scripts\build-desktop.bat fresh in build\fc.
rem (emsdk is NOT simulated with a junction: see fc-04a - Emscripten keeps its cache inside the real
rem emsdk folder; the wasm override test uses a separate real emsdk install in build\em, step fc-07.)
rem Prints the tool paths the resulting CMake cache uses. Junctions stay for step 6 (removed at cleanup).
rem Exit 0 only when every step returned 0.
pushd "%~dp0..\..\..\.." || exit /b 1
set "TF=%CD%"
popd
set "T=%TF%\build\fc-tools"
if not exist "%T%" mkdir "%T%"
if not exist "%T%\Qt" mklink /J "%T%\Qt" "C:\Qt"
if not exist "%T%\VS" mklink /J "%T%\VS" "C:\Program Files\Microsoft Visual Studio\18\Community"
set "TAIDAFLOW_QT_ROOT=%T%\Qt\6.8.3"
set "TAIDAFLOW_QT_TOOLS=%T%\Qt\Tools"
set "TAIDAFLOW_VCVARS64=%T%\VS\VC\Auxiliary\Build\vcvars64.bat"
set TAIDAFLOW_EMSDK=
set TAIDAFLOW_NO_PRESET=
set TAIDAFLOW_
cd /d "%TF%\build\fc" || exit /b 1
echo [w2-063] cwd=%CD%
call scripts\build-desktop.bat fresh
echo [w2-063] step build-desktop.bat-fresh(override) exit=%ERRORLEVEL%
if errorlevel 1 exit /b 41
echo [w2-063] build\fc\build\desktop\CMakeCache.txt:
findstr /b "CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM CMAKE_CXX_COMPILER:" build\desktop\CMakeCache.txt
echo [w2-063] CMakeCache lines naming the default locations (C:/Qt/, Visual Studio/18):
findstr /i /c:"C:/Qt/" /c:"Visual Studio/18" build\desktop\CMakeCache.txt
echo [w2-063] (end of list)
dir build\desktop\TaidaFlowApp.exe | findstr /i "TaidaFlowApp"
exit /b 0
