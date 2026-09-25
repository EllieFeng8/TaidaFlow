@echo off
rem w2-045: configure, build and run the History step/keyset QTest harness (Release, MSVC 2022 x64)
rem and build the D3 measurement program w2045_bench.exe (run by run_bench.py, not by CTest).
rem Needs the w2-041 bench data: python -B docs\evidence\w2-041\tools\make_bench_db.py
setlocal
set TF=%~dp0..\..\..\..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
set QT_FORCE_STDERR_LOGGING=1
rem the OFFSET reference checks may take longer than the default 5 min per test function
set QTEST_FUNCTION_TIMEOUT=1800000
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0qtest" -B "%TF%\build\w2-045-qtest" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-045-qtest" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%TF%\build\w2-045-qtest" -V %*
exit /b %ERRORLEVEL%
