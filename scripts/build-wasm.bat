@echo off
rem TaidaFlow WebAssembly build (Qt 6.8.3 wasm_singlethread + emsdk 3.1.56) -> build\<preset>.
rem Usage: scripts\build-wasm.bat [wasm-release|wasm-debug] [fresh]
rem   default preset: wasm-release -> build\wasm-release
rem Exit code: 0 = success, 1 = a tool was not found or cmake failed.
rem
rem Tool locations (docs\BUILD.md section 2.6). Environment variables; not set = the default:
rem   TAIDAFLOW_QT_ROOT    Qt version folder (wasm_singlethread + msvc2022_64 as host)  default C:\Qt\6.8.3
rem   TAIDAFLOW_QT_TOOLS   Qt Tools folder (CMake_64, Ninja)                            default C:\Qt\Tools
rem   TAIDAFLOW_EMSDK      emsdk folder with 3.1.56 installed and activated             default C:\tools\emsdk
rem All three at their defaults and emsdk's bundled Python 3.9.2-nuget / Node 16.20.0 present (the
rem paths written in CMakePresets.json) -> CMakePresets.json preset. Otherwise (or
rem TAIDAFLOW_NO_PRESET=1) the same configuration is passed on the command line with the paths from
rem the variables and from emsdk_env (EMSDK_NODE), into the same build\<preset> folder (only for
rem wasm-release and wasm-debug; any other preset name, e.g. one from your own
rem CMakeUserPresets.json, is always used as a preset).
rem After changing tool paths for an existing build folder, run once with "fresh".
setlocal
set "ROOT=%~dp0.."
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=wasm-release"

if not defined TAIDAFLOW_QT_ROOT set "TAIDAFLOW_QT_ROOT=C:\Qt\6.8.3"
if not defined TAIDAFLOW_QT_TOOLS set "TAIDAFLOW_QT_TOOLS=C:\Qt\Tools"
if not defined TAIDAFLOW_EMSDK set "TAIDAFLOW_EMSDK=C:\tools\emsdk"

set "USE_PRESET=1"
if /I not "%TAIDAFLOW_QT_ROOT%"=="C:\Qt\6.8.3" set "USE_PRESET=0"
if /I not "%TAIDAFLOW_QT_TOOLS%"=="C:\Qt\Tools" set "USE_PRESET=0"
if /I not "%TAIDAFLOW_EMSDK%"=="C:\tools\emsdk" set "USE_PRESET=0"
if not exist "%TAIDAFLOW_EMSDK%\python\3.9.2-nuget_64bit\python.exe" set "USE_PRESET=0"
if not exist "%TAIDAFLOW_EMSDK%\node\16.20.0_64bit\bin\node.exe" set "USE_PRESET=0"
if "%TAIDAFLOW_NO_PRESET%"=="1" set "USE_PRESET=0"

set "CMAKE=%TAIDAFLOW_QT_TOOLS%\CMake_64\bin\cmake.exe"
set "NINJA=%TAIDAFLOW_QT_TOOLS%\Ninja\ninja.exe"
set "QT_TOOLCHAIN=%TAIDAFLOW_QT_ROOT%\wasm_singlethread\lib\cmake\Qt6\qt.toolchain.cmake"
set "QT_HOST=%TAIDAFLOW_QT_ROOT%\msvc2022_64"
set "EM_TOOLCHAIN=%TAIDAFLOW_EMSDK%\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"

if not exist "%TAIDAFLOW_EMSDK%\emsdk_env.bat" (
    echo [build-wasm] emsdk not found: "%TAIDAFLOW_EMSDK%\emsdk_env.bat" - set TAIDAFLOW_EMSDK ^(docs\BUILD.md 2.6^)
    exit /b 1
)
if not exist "%EM_TOOLCHAIN%" (
    echo [build-wasm] Emscripten not installed in "%TAIDAFLOW_EMSDK%" - run: emsdk install 3.1.56 ^& emsdk activate 3.1.56 ^(docs\BUILD.md 2.4^)
    exit /b 1
)
if not exist "%CMAKE%" (
    echo [build-wasm] cmake.exe not found: "%CMAKE%" - set TAIDAFLOW_QT_TOOLS ^(docs\BUILD.md 2.6^)
    exit /b 1
)
if not exist "%NINJA%" (
    echo [build-wasm] ninja.exe not found: "%NINJA%" - set TAIDAFLOW_QT_TOOLS ^(docs\BUILD.md 2.6^)
    exit /b 1
)
if not exist "%QT_TOOLCHAIN%" (
    echo [build-wasm] Qt wasm_singlethread kit not found: "%QT_TOOLCHAIN%" - set TAIDAFLOW_QT_ROOT ^(docs\BUILD.md 2.6^)
    exit /b 1
)
if not exist "%QT_HOST%\bin\qtpaths.exe" (
    echo [build-wasm] Qt msvc2022_64 kit ^(host tools^) not found: "%QT_HOST%" - set TAIDAFLOW_QT_ROOT ^(docs\BUILD.md 2.6^)
    exit /b 1
)

call "%TAIDAFLOW_EMSDK%\emsdk_env.bat" >nul 2>&1

rem Any other preset name (for example one of your own in CMakeUserPresets.json) is always
rem configured and built with that preset.
set "BUILD_TYPE="
if /I "%PRESET%"=="wasm-release" set "BUILD_TYPE=Release"
if /I "%PRESET%"=="wasm-debug" set "BUILD_TYPE=Debug"
if not defined BUILD_TYPE set "USE_PRESET=1"

pushd "%ROOT%" || exit /b 1
if /I "%~2"=="fresh" if exist "build\%PRESET%" rmdir /s /q "build\%PRESET%"
if "%USE_PRESET%"=="1" (
    "%CMAKE%" --preset %PRESET% || (popd & exit /b 1)
    "%CMAKE%" --build --preset %PRESET% || (popd & exit /b 1)
) else (
    echo [build-wasm] tool paths from TAIDAFLOW_QT_ROOT="%TAIDAFLOW_QT_ROOT%" TAIDAFLOW_QT_TOOLS="%TAIDAFLOW_QT_TOOLS%" TAIDAFLOW_EMSDK="%TAIDAFLOW_EMSDK%" ^(no preset^)
    "%CMAKE%" -S . -B "build\%PRESET%" -G Ninja "-DCMAKE_BUILD_TYPE=%BUILD_TYPE%" "-DCMAKE_MAKE_PROGRAM=%NINJA:\=/%" "-DCMAKE_TOOLCHAIN_FILE=%QT_TOOLCHAIN:\=/%" "-DQT_CHAINLOAD_TOOLCHAIN_FILE=%EM_TOOLCHAIN:\=/%" "-DEMSCRIPTEN_ROOT_PATH=%TAIDAFLOW_EMSDK:\=/%/upstream/emscripten" "-DQT_HOST_PATH=%QT_HOST:\=/%" "-DQT_HOST_PATH_CMAKE_DIR=%QT_HOST:\=/%/lib/cmake" "-DCMAKE_CROSSCOMPILING_EMULATOR=%EMSDK_NODE:\=/%" -DBUILD_TESTING=OFF || (popd & exit /b 1)
    "%CMAKE%" --build "build\%PRESET%" --target TaidaFlowApp || (popd & exit /b 1)
)
popd
exit /b 0
