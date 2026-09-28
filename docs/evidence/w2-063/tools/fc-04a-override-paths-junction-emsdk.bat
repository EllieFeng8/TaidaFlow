@echo off
rem w2-063 fresh-clone step 4 (BUILD.md 2.6, tools in other locations): junctions under build\fc-tools
rem point to the real Qt, Visual Studio and emsdk; the TAIDAFLOW_* variables name the junction
rem paths; then scripts\build-desktop.bat fresh and scripts\build-wasm.bat wasm-release fresh in build\fc.
rem Prints the tool paths the resulting CMake caches use. Junctions stay for step 6 (removed in step 7).
rem Exit 0 only when every step returned 0.
pushd "%~dp0..\..\..\.." || exit /b 1
set "TF=%CD%"
popd
set "T=%TF%\build\fc-tools"
if not exist "%T%" mkdir "%T%"
if not exist "%T%\Qt" mklink /J "%T%\Qt" "C:\Qt"
if not exist "%T%\VS" mklink /J "%T%\VS" "C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%T%\emsdk" mklink /J "%T%\emsdk" "C:\tools\emsdk"
set "TAIDAFLOW_QT_ROOT=%T%\Qt\6.8.3"
set "TAIDAFLOW_QT_TOOLS=%T%\Qt\Tools"
set "TAIDAFLOW_VCVARS64=%T%\VS\VC\Auxiliary\Build\vcvars64.bat"
set "TAIDAFLOW_EMSDK=%T%\emsdk"
set TAIDAFLOW_NO_PRESET=
set TAIDAFLOW_
cd /d "%TF%\build\fc" || exit /b 1
echo [w2-063] cwd=%CD%
call scripts\build-desktop.bat fresh
echo [w2-063] step build-desktop.bat-fresh(override) exit=%ERRORLEVEL%
if errorlevel 1 exit /b 41
call scripts\build-wasm.bat wasm-release fresh
echo [w2-063] step build-wasm.bat-fresh(override) exit=%ERRORLEVEL%
if errorlevel 1 exit /b 42
echo [w2-063] build\fc\build\desktop\CMakeCache.txt:
findstr /b "CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM CMAKE_CXX_COMPILER:" build\desktop\CMakeCache.txt
echo [w2-063] build\fc\build\wasm-release\CMakeCache.txt:
findstr /b "CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM QT_HOST_PATH QT_CHAINLOAD_TOOLCHAIN_FILE EMSCRIPTEN_ROOT_PATH CMAKE_CROSSCOMPILING_EMULATOR CMAKE_CXX_COMPILER:" build\wasm-release\CMakeCache.txt
echo [w2-063] CMakeCache lines naming the default locations (C:/Qt/, C:/tools/emsdk, Visual Studio/18):
findstr /i /c:"C:/Qt/" /c:"C:/tools/emsdk" /c:"Visual Studio/18" build\desktop\CMakeCache.txt build\wasm-release\CMakeCache.txt
echo [w2-063] (end of list)
dir build\desktop\TaidaFlowApp.exe build\wasm-release\TaidaFlowApp.wasm | findstr /i "TaidaFlowApp"
exit /b 0
