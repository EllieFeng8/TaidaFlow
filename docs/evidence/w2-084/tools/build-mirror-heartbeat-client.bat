@echo off
rem w2-084: build the dev-only Proxy Mirror heartbeat client (Release, MSVC 2022 x64, Qt 6.8.3)
rem into taidaflow\build\w2-084-mirror-heartbeat-client\mirror_heartbeat_client.exe
setlocal
set TF=%~dp0..\..\..\..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\Tools\Ninja;%PATH%
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0mirror-heartbeat-client" -B "%TF%\build\w2-084-mirror-heartbeat-client" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-084-mirror-heartbeat-client" || exit /b 1
exit /b 0
