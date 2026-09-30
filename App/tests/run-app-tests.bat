@echo off
rem App unit tests (App\tests, QTest + CTest: tst_appconfig, tst_runtimeinfo, tst_applog):
rem configure, build and run with MSVC x64 / Qt 6.8.3 msvc2022_64 (Release) into build\app-tests.
rem Compiles the application's own App\ sources; no device, no fixed port, data in temporary folders.
rem See App\tests\README.md.  (w2-077: replaces the developer-PC-only build-tests.bat of w2-064.)
rem
rem Usage (from any folder):  App\tests\run-app-tests.bat [fresh] [extra ctest arguments]
rem   fresh = delete build\app-tests first.  Exit code = cmake's on a configure/build error, else ctest's
rem   (0 = all tests passed).
rem
rem Tool locations (docs\BUILD.md section 2.6), same variables as scripts\build-desktop.bat:
rem   TAIDAFLOW_QT_ROOT    default C:\Qt\6.8.3            (uses <root>\msvc2022_64)
rem   TAIDAFLOW_QT_TOOLS   default C:\Qt\Tools            (CMake_64\bin\cmake.exe, ctest.exe, Ninja\ninja.exe)
rem   TAIDAFLOW_VCVARS64   default C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
setlocal
set "TF=%~dp0..\.."
set "BUILD=%TF%\build\app-tests"
if not defined TAIDAFLOW_QT_ROOT set "TAIDAFLOW_QT_ROOT=C:\Qt\6.8.3"
if not defined TAIDAFLOW_QT_TOOLS set "TAIDAFLOW_QT_TOOLS=C:\Qt\Tools"
if not defined TAIDAFLOW_VCVARS64 set "TAIDAFLOW_VCVARS64=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "QTDIR=%TAIDAFLOW_QT_ROOT%\msvc2022_64"
set "CMAKE=%TAIDAFLOW_QT_TOOLS%\CMake_64\bin\cmake.exe"
set "CTEST=%TAIDAFLOW_QT_TOOLS%\CMake_64\bin\ctest.exe"
set "NINJA=%TAIDAFLOW_QT_TOOLS%\Ninja\ninja.exe"
for %%F in ("%TAIDAFLOW_VCVARS64%" "%CMAKE%" "%CTEST%" "%NINJA%" "%QTDIR%\bin\qtpaths.exe") do (
    if not exist "%%~F" (
        echo [run-app-tests] not found: %%~F - see docs\BUILD.md 2.6 ^(TAIDAFLOW_QT_ROOT / TAIDAFLOW_QT_TOOLS / TAIDAFLOW_VCVARS64^)
        exit /b 1
    )
)
call "%TAIDAFLOW_VCVARS64%" >nul || exit /b 1
set "VSLANG=1033"
if /I "%~1"=="fresh" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    shift
)
"%CMAKE%" -S "%TF%\App\tests" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA:\=/%" "-DCMAKE_PREFIX_PATH=%QTDIR:\=/%" || exit /b 1
"%CMAKE%" --build "%BUILD%" || exit /b 1
"%CTEST%" --test-dir "%BUILD%" --output-on-failure %1 %2 %3 %4 %5 %6
exit /b %ERRORLEVEL%
