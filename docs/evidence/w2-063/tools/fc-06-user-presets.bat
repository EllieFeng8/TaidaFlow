@echo off
rem w2-063 fresh-clone step 6 (BUILD.md 2.6, CMakeUserPresets.json template): in build\fc, write
rem CMakeUserPresets.json from the BUILD.md template (Qt = build\fc-tools\Qt junction, emsdk = separate real
rem install build\em after "git checkout 3.1.56": node 16.20.0 / python 3.9.2-nuget), delete the
rem clone's build\desktop and build\wasm-release, then build with the user presets as documented:
rem  1) new cmd: vcvars64 (junction) + cmake --preset my-desktop-release + cmake --build --preset my-desktop-release
rem  2) new cmd: emsdk_env (build\em) + cmake --preset my-wasm-release + cmake --build --preset my-wasm-release
rem  3) scripts\build-wasm.bat my-wasm-release (user preset name passed through; incremental)
rem Exit 0 only when every step returned 0.
pushd "%~dp0..\..\..\.." || exit /b 1
set "TF=%CD%"
popd
set "T=%TF%\build\fc-tools"
cd /d "%TF%\build\fc" || exit /b 1
py -3 -B "%~dp0fc-06-make-user-presets.py" . "%T%" "%TF%\build\em"
echo [w2-063] step write-CMakeUserPresets exit=%ERRORLEVEL%
if errorlevel 1 exit /b 60
if exist build\desktop rmdir /s /q build\desktop
if exist build\wasm-release rmdir /s /q build\wasm-release
cmd /c "call "%T%\VS\VC\Auxiliary\Build\vcvars64.bat" && "%T%\Qt\Tools\CMake_64\bin\cmake.exe" --preset my-desktop-release && "%T%\Qt\Tools\CMake_64\bin\cmake.exe" --build --preset my-desktop-release"
echo [w2-063] step user-preset desktop exit=%ERRORLEVEL%
if errorlevel 1 exit /b 61
cmd /c "call "%TF%\build\em\emsdk_env.bat" && "%T%\Qt\Tools\CMake_64\bin\cmake.exe" --preset my-wasm-release && "%T%\Qt\Tools\CMake_64\bin\cmake.exe" --build --preset my-wasm-release"
echo [w2-063] step user-preset wasm exit=%ERRORLEVEL%
if errorlevel 1 exit /b 62
cmd /c "set "TAIDAFLOW_QT_ROOT=%T%\Qt\6.8.3"&& set "TAIDAFLOW_QT_TOOLS=%T%\Qt\Tools"&& set "TAIDAFLOW_EMSDK=%TF%\build\em"&& scripts\build-wasm.bat my-wasm-release"
echo [w2-063] step build-wasm.bat my-wasm-release exit=%ERRORLEVEL%
if errorlevel 1 exit /b 63
echo [w2-063] build\fc\build\desktop\CMakeCache.txt:
findstr /b "CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM CMAKE_CXX_COMPILER:" build\desktop\CMakeCache.txt
echo [w2-063] build\fc\build\wasm-release\CMakeCache.txt:
findstr /b "CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM QT_HOST_PATH QT_CHAINLOAD_TOOLCHAIN_FILE EMSCRIPTEN_ROOT_PATH CMAKE_CROSSCOMPILING_EMULATOR CMAKE_CXX_COMPILER:" build\wasm-release\CMakeCache.txt
echo [w2-063] CMakeCache lines naming the default locations (C:/Qt/, C:/tools/emsdk, Visual Studio/18):
findstr /i /c:"C:/Qt/" /c:"C:/tools/emsdk" /c:"Visual Studio/18" build\desktop\CMakeCache.txt build\wasm-release\CMakeCache.txt
echo [w2-063] (end of list)
dir build\desktop\TaidaFlowApp.exe build\wasm-release\TaidaFlowApp.wasm | findstr /i "TaidaFlowApp"
exit /b 0
