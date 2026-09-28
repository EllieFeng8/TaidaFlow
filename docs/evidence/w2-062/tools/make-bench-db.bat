@echo off
rem w2-062: build and run the C++ bench data generator (tools\bench-db, no Python) ->
rem taidaflow\build\w2-041-bench\data (the data folder the w2-041/w2-049, w2-045 and w2-052 QTests use).
rem Usage: docs\evidence\w2-062\tools\make-bench-db.bat [--rebuild]
setlocal
set TF=%~dp0..\..\..\..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0bench-db" -B "%TF%\build\w2-062-bench-tool" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-062-bench-tool" || exit /b 1
"%TF%\build\w2-062-bench-tool\make_bench_db.exe" "%TF%\build\w2-041-bench\data" %*
exit /b %ERRORLEVEL%
