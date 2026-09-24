@echo off
rem w2-041: configure, build and run the export/range QTest harness (Release, MSVC 2022 x64).
rem Needs the bench data: python docs\evidence\w2-041\tools\make_bench_db.py
rem Port 8124 must be free (the download-service test binds it); the script refuses otherwise.
setlocal
set TF=%~dp0..\..\..\..
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
set QT_FORCE_STDERR_LOGGING=1
set QT_QPA_PLATFORM=offscreen
powershell -NoProfile -Command "if (Get-NetTCPConnection -State Listen -LocalPort 8124 -ErrorAction SilentlyContinue) { 'port 8124 is in use - not running'; exit 3 }" || exit /b 3
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0qtest" -B "%TF%\build\w2-041-qtest" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%TF%\build\w2-041-qtest" || exit /b 1
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir "%TF%\build\w2-041-qtest" -V %*
exit /b %ERRORLEVEL%
