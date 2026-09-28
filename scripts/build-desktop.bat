@echo off
rem TaidaFlow desktop (MSVC x64, Qt 6.8.3 msvc2022_64) configure + build -> build\desktop.
rem Usage: scripts\build-desktop.bat          -> build\desktop (incremental)
rem        scripts\build-desktop.bat fresh    -> wipe build\desktop first
rem Exit code: 0 = success, 1 = a tool was not found or cmake failed.
rem
rem Tool locations (docs\BUILD.md section 2.6). Environment variables; not set = the default:
rem   TAIDAFLOW_QT_ROOT    Qt version folder (holds msvc2022_64)   default C:\Qt\6.8.3
rem   TAIDAFLOW_QT_TOOLS   Qt Tools folder (CMake_64, Ninja)        default C:\Qt\Tools
rem   TAIDAFLOW_VCVARS64   vcvars64.bat of Visual Studio            default C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
rem Qt root and Qt Tools at their defaults -> CMakePresets.json preset "desktop-release" (the paths
rem written in the preset). Otherwise (or TAIDAFLOW_NO_PRESET=1) the same configuration is passed
rem on the command line with the paths from the variables, into the same build\desktop folder.
rem After changing tool paths for an existing build\desktop, run once with "fresh".
setlocal
set "ROOT=%~dp0.."

if not defined TAIDAFLOW_QT_ROOT set "TAIDAFLOW_QT_ROOT=C:\Qt\6.8.3"
if not defined TAIDAFLOW_QT_TOOLS set "TAIDAFLOW_QT_TOOLS=C:\Qt\Tools"
if not defined TAIDAFLOW_VCVARS64 set "TAIDAFLOW_VCVARS64=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

set "USE_PRESET=1"
if /I not "%TAIDAFLOW_QT_ROOT%"=="C:\Qt\6.8.3" set "USE_PRESET=0"
if /I not "%TAIDAFLOW_QT_TOOLS%"=="C:\Qt\Tools" set "USE_PRESET=0"
if "%TAIDAFLOW_NO_PRESET%"=="1" set "USE_PRESET=0"

set "CMAKE=%TAIDAFLOW_QT_TOOLS%\CMake_64\bin\cmake.exe"
set "NINJA=%TAIDAFLOW_QT_TOOLS%\Ninja\ninja.exe"
set "QT_TOOLCHAIN=%TAIDAFLOW_QT_ROOT%\msvc2022_64\lib\cmake\Qt6\qt.toolchain.cmake"

if not exist "%TAIDAFLOW_VCVARS64%" (
    echo [build-desktop] vcvars64.bat not found: "%TAIDAFLOW_VCVARS64%" - set TAIDAFLOW_VCVARS64 ^(docs\BUILD.md 2.6^)
    exit /b 1
)
if not exist "%CMAKE%" (
    echo [build-desktop] cmake.exe not found: "%CMAKE%" - set TAIDAFLOW_QT_TOOLS ^(docs\BUILD.md 2.6^)
    exit /b 1
)
if not exist "%NINJA%" (
    echo [build-desktop] ninja.exe not found: "%NINJA%" - set TAIDAFLOW_QT_TOOLS ^(docs\BUILD.md 2.6^)
    exit /b 1
)
if not exist "%QT_TOOLCHAIN%" (
    echo [build-desktop] Qt msvc2022_64 kit not found: "%QT_TOOLCHAIN%" - set TAIDAFLOW_QT_ROOT ^(docs\BUILD.md 2.6^)
    exit /b 1
)

call "%TAIDAFLOW_VCVARS64%" >nul || exit /b 1

pushd "%ROOT%" || exit /b 1
if /I "%~1"=="fresh" if exist "build\desktop" rmdir /s /q "build\desktop"
if "%USE_PRESET%"=="1" (
    "%CMAKE%" --preset desktop-release || (popd & exit /b 1)
    "%CMAKE%" --build --preset desktop-release || (popd & exit /b 1)
) else (
    echo [build-desktop] tool paths from TAIDAFLOW_QT_ROOT="%TAIDAFLOW_QT_ROOT%" TAIDAFLOW_QT_TOOLS="%TAIDAFLOW_QT_TOOLS%" ^(no preset^)
    set "VSLANG=1033"
    "%CMAKE%" -S . -B build\desktop -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA:\=/%" "-DCMAKE_TOOLCHAIN_FILE=%QT_TOOLCHAIN:\=/%" || (popd & exit /b 1)
    "%CMAKE%" --build build\desktop --target TaidaFlowApp || (popd & exit /b 1)
)
popd
exit /b 0
