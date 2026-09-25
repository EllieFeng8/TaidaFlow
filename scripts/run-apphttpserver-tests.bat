@echo off
rem w2-049: configure, build and run the stand-alone AppHttpServer QTest (Core\AppHttpServer\tests)
rem with MSVC 2022 x64 / Qt 6.8.3 (Release).  Only Core\AppHttpServer\AppHttpServer.{h,cpp} and the
rem test are compiled; no other TaidaFlow source.  Uses free ports picked by the OS (no fixed port).
rem Needs about 2 GiB free disk space for the two temporary 1 GiB test files (removed afterwards).
rem Usage: scripts\run-apphttpserver-tests.bat [extra ctest arguments]
setlocal
set TF=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
set QT_FORCE_STDERR_LOGGING=1
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%TF%\Core\AppHttpServer\tests" -B "%TF%\build\apphttpserver-qtest" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\apphttpserver-qtest" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%TF%\build\apphttpserver-qtest" -V %*
exit /b %ERRORLEVEL%
