@echo off
rem w2-090: App\tests (QTest + CTest) built fresh into the task's own folder build\w2-090-run\app-tests.
rem Same commands as docs\evidence\w2-089\tools\run-tests-w2089.bat app fresh (MSVC 2022 x64,
rem Qt 6.8.3 msvc2022_64, Release); only the build folder differs.
rem Usage (any folder): app-tests-w2090.bat [extra ctest arguments]      Exit code = ctest exit code.
setlocal
set "TF=%~dp0..\..\..\.."
set "SRC=%TF%\App\tests"
set "BUILD=%TF%\build\w2-090-run\app-tests"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "VSLANG=1033"
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%"
if exist "%BUILD%" rmdir /s /q "%BUILD%"
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%BUILD%" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%BUILD%" --output-on-failure %1 %2 %3 %4 %5 %6
exit /b %ERRORLEVEL%
