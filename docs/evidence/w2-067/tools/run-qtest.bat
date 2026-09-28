@echo off
rem w2-067: configure, build and run the SqlManager::shutdown() QTest (Release, MSVC 2022 x64) -> build\w2-067-qtest.
rem No device, no network: temporary data folder in the build tree.
setlocal
set TF=%~dp0..\..\..\..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
set QT_FORCE_STDERR_LOGGING=1
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0qtest" -B "%TF%\build\w2-067-qtest" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-067-qtest" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%TF%\build\w2-067-qtest" -V %*
exit /b %ERRORLEVEL%
