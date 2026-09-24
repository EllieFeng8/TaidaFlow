@echo off
rem TaidaFlow WebAssembly build (Qt 6.8.3 wasm_singlethread + emsdk 3.1.56) via CMakePresets.
rem Usage: scripts\build-wasm.bat [wasm-release|wasm-debug] [fresh]
rem   default preset: wasm-release -> build\wasm-release
rem Exit code = cmake exit code (0 = success).
setlocal
set "ROOT=%~dp0.."
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=wasm-release"

call C:\tools\emsdk\emsdk_env.bat >nul 2>&1

pushd "%ROOT%" || exit /b 1
if /I "%~2"=="fresh" if exist "build\%PRESET%" rmdir /s /q "build\%PRESET%"
"%CMAKE%" --preset %PRESET% || (popd & exit /b 1)
"%CMAKE%" --build --preset %PRESET% || (popd & exit /b 1)
popd
exit /b 0
