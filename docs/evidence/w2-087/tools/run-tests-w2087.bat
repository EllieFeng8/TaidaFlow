@echo off
rem w2-087: Core\tests or App\tests (QTest + CTest) built into the task's own folder
rem build\w2-087-run\<core-tests|app-tests> - same commands as Core\tests\run-core-tests.bat /
rem App\tests\run-app-tests.bat (MSVC 2022 x64, Qt 6.8.3 msvc2022_64, Release), only the build folder
rem differs (build\core-tests / build\app-tests of others are not touched).
rem Usage: run-tests-w2087.bat core|app [fresh] [extra ctest arguments]
setlocal
set "TF=%~dp0..\..\..\.."
set "KIND=%~1"
shift
if /I "%KIND%"=="core" (
    set "SRC=%TF%\Core\tests"
    set "BUILD=%TF%\build\w2-087-run\core-tests"
) else if /I "%KIND%"=="app" (
    set "SRC=%TF%\App\tests"
    set "BUILD=%TF%\build\w2-087-run\app-tests"
) else (
    echo usage: run-tests-w2087.bat core^|app [fresh] [ctest arguments]
    exit /b 2
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "VSLANG=1033"
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%"
if /I "%~1"=="fresh" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    shift
)
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%BUILD%" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%BUILD%" --output-on-failure %1 %2 %3 %4 %5 %6
exit /b %ERRORLEVEL%
