@echo off
rem w2-063 fresh-clone step 7 (BUILD.md 2.6, tools in other locations, WebAssembly): Qt through the
rem build\fc-tools\Qt junction, emsdk = the separate real installation in build\em (em-01);
rem scripts\build-wasm.bat wasm-release fresh in build\fc. Arg 1 = label for the log.
rem Prints the tool paths the resulting CMake cache uses. Exit 0 only when every step returned 0.
pushd "%~dp0..\..\..\.." || exit /b 1
set "TF=%CD%"
popd
set "T=%TF%\build\fc-tools"
if not exist "%T%\Qt" (echo [w2-063] run fc-04 first & exit /b 2)
set "TAIDAFLOW_QT_ROOT=%T%\Qt\6.8.3"
set "TAIDAFLOW_QT_TOOLS=%T%\Qt\Tools"
set "TAIDAFLOW_EMSDK=%TF%\build\em"
set TAIDAFLOW_VCVARS64=
set TAIDAFLOW_NO_PRESET=
set TAIDAFLOW_
cd /d "%TF%\build\fc" || exit /b 1
echo [w2-063] %~1 cwd=%CD%
git -C "%TAIDAFLOW_EMSDK%" log --oneline -1
dir /b "%TAIDAFLOW_EMSDK%\node" "%TAIDAFLOW_EMSDK%\python"
call scripts\build-wasm.bat wasm-release fresh
echo [w2-063] step build-wasm.bat-fresh(override) exit=%ERRORLEVEL%
if errorlevel 1 exit /b 71
echo [w2-063] build\fc\build\wasm-release\CMakeCache.txt:
findstr /b "CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM QT_HOST_PATH QT_CHAINLOAD_TOOLCHAIN_FILE EMSCRIPTEN_ROOT_PATH CMAKE_CROSSCOMPILING_EMULATOR CMAKE_CXX_COMPILER:" build\wasm-release\CMakeCache.txt
echo [w2-063] CMakeCache lines naming the default locations (C:/Qt/, C:/tools/emsdk):
findstr /i /c:"C:/Qt/" /c:"C:/tools/emsdk" build\wasm-release\CMakeCache.txt
echo [w2-063] (end of list)
echo [w2-063] Emscripten cache sanity file of build\em:
type "%TAIDAFLOW_EMSDK%\upstream\emscripten\cache\sanity.txt"
echo.
dir build\wasm-release\TaidaFlowApp.html build\wasm-release\TaidaFlowApp.js build\wasm-release\TaidaFlowApp.wasm | findstr /i "TaidaFlowApp"
exit /b 0
