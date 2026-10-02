@echo off
rem w2-085: build the dev-only live test tools (mirror_limit_client, alarm_dump; Release, MSVC 2022 x64,
rem Qt 6.8.3) into taidaflow\build\w2-085-run\tools
setlocal
set "TF=%~dp0..\..\..\.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Qt\Tools\Ninja;%PATH%"
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0mirror-limit-client" -B "%TF%\build\w2-085-run\tools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-085-run\tools" || exit /b 1
exit /b 0
