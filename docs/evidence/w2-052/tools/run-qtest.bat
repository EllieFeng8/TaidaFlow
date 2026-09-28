@echo off
rem w2-052: configure, build and run the per-client History view QTests (Release, MSVC 2022 x64):
rem tst_w2052_views (HistoryViewService + SqlManager + Proxy) and tst_w2052_rangepage (the w2-045
rem correctness test through the per-session SqlManager API).  No network port is used.
rem Needs the w2-041 bench data: docs\evidence\w2-062\tools\make-bench-db.bat (C++ generator, no Python; writes build\w2-041-bench\data)
setlocal
set TF=%~dp0..\..\..\..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
set QT_FORCE_STDERR_LOGGING=1
set QT_QPA_PLATFORM=offscreen
rem the OFFSET reference checks may take longer than the default 5 min per test function
set QTEST_FUNCTION_TIMEOUT=1800000
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0qtest" -B "%TF%\build\w2-052-qtest" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-052-qtest" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%TF%\build\w2-052-qtest" -V %*
exit /b %ERRORLEVEL%
