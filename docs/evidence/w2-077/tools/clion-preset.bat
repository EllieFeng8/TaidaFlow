@echo off
rem w2-077 D5: a CLion "preset" profile on the command line (docs\BUILD.md 6A.3 B) with CLion's bundled
rem cmake (4.3.1). desktop-release runs inside vcvars64 (CLion toolchain "Visual Studio"); wasm-release
rem runs after emsdk_env.bat (BUILD.md 5.2). The preset's settings are used as they are; only the
rem binary folder is moved to build\w2-077-<preset> (-B) so that build\desktop / build\wasm-release,
rem which the packaging reads, are not touched while other work runs. The build step builds the same
rem target as the build preset (TaidaFlowApp).
rem Usage: clion-preset.bat desktop-release|wasm-release [fresh]      Exit code = cmake's.
setlocal
if not defined CLION set "CLION=%LOCALAPPDATA%\Programs\CLion"
set "CMAKE=%CLION%\bin\cmake\win\x64\bin\cmake.exe"
for %%I in ("%~dp0..\..\..\..") do set "SRC=%%~fI"
set "PRESET=%~1"
if "%PRESET%"=="" (echo usage: clion-preset.bat desktop-release^|wasm-release [fresh] & exit /b 2)
if not exist "%CMAKE%" (echo [clion-preset] not found: %CMAKE% - set CLION & exit /b 2)
set "BIN=build\w2-077-%PRESET%"
set "FRESH="
if /i "%~2"=="fresh" set "FRESH=--fresh"
cd /d "%SRC%" || exit /b 2
if /i not "%PRESET:~0,4%"=="wasm" goto vcvars
call C:\tools\emsdk\emsdk_env.bat >nul
echo [clion-preset] emsdk_env exit=%ERRORLEVEL%
goto env_done
:vcvars
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
echo [clion-preset] vcvars64 exit=%ERRORLEVEL%
:env_done
"%CMAKE%" --version | findstr /b "cmake version"
echo [clion-preset] %DATE% %TIME% configure start: cmake --preset %PRESET% -B %BIN% %FRESH%
"%CMAKE%" --preset %PRESET% -B %BIN% %FRESH%
set RC=%ERRORLEVEL%
echo [clion-preset] %DATE% %TIME% configure exit=%RC%
if not "%RC%"=="0" exit /b %RC%
echo [clion-preset] %DATE% %TIME% build start: cmake --build %BIN% --target TaidaFlowApp
"%CMAKE%" --build %BIN% --target TaidaFlowApp
set RC=%ERRORLEVEL%
echo [clion-preset] %DATE% %TIME% build exit=%RC%
exit /b %RC%
