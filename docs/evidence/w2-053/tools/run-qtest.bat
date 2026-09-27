@echo off
rem w2-053: configure, build and run the DI restart hand-over QTest harness (Release, MSVC 2022 x64).
rem No device, no network: the test feeds DI samples into the real Manager through the
rem ModbusClient::registersRead signal and uses a temporary data folder in the build tree.
setlocal
set TF=%~dp0..\..\..\..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
set QT_FORCE_STDERR_LOGGING=1
set QT_QPA_PLATFORM=offscreen
set TAIDAFLOW_DEVICE_PROFILE=simulator
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0qtest" -B "%TF%\build\w2-053-qtest" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-053-qtest" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%TF%\build\w2-053-qtest" -V %*
exit /b %ERRORLEVEL%
