@echo off
rem w2-067: builds the SQLite check program into build\w2-067-dbcheck (Release, MSVC x64, Qt 6.8.3).
setlocal
set "ROOT=%~dp0..\..\..\.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
"C:\Qt\Tools\CMake_64\bin\cmake.exe" -S "%~dp0dbcheck" -B "%ROOT%\build\w2-067-dbcheck" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || exit /b 1
"C:\Qt\Tools\CMake_64\bin\cmake.exe" --build "%ROOT%\build\w2-067-dbcheck" || exit /b 1
exit /b 0
