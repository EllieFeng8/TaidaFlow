@echo off
rem Core unit tests (Core\tests, QTest + CTest): configure, build and run with MSVC 2022 x64 /
rem Qt 6.8.3 (Release) into build\core-tests. Compiles the application's own Core sources; no
rem device, no fixed port, data in temporary folders. See README.md (section on the Core unit tests).
rem Usage: Core\tests\run-core-tests.bat [fresh] [extra ctest arguments]
setlocal
set "TF=%~dp0..\.."
set "BUILD=%TF%\build\core-tests"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%"
if /I "%~1"=="fresh" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    shift
)
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%TF%\Core\tests" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%BUILD%" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%BUILD%" --output-on-failure %1 %2 %3 %4 %5 %6
exit /b %ERRORLEVEL%
