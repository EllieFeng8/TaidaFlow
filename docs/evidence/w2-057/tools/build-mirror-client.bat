@echo off
rem w2-057: build the dev-only Proxy Mirror export client (source: docs\evidence\w2-050\tools\mirror-export-client,
rem unchanged) against the CURRENT Core/TaidaFlowProxy.h into taidaflow\build\w2-057-mirror-client.
setlocal
set "TF=%~dp0..\..\..\.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\Tools\Ninja;%PATH%
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%TF%\docs\evidence\w2-050\tools\mirror-export-client" -B "%TF%\build\w2-057-mirror-client" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-057-mirror-client" || exit /b 1
exit /b 0
