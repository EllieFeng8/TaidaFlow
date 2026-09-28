@echo off
rem w2-067 triage: RelWithDebInfo desktop build (same CMake project / options as build-desktop.bat, plus PDB).
rem Usage: docs\evidence\w2-067\tools\build-dbg.bat [fresh|incremental] [<source dir> <build dir>]
rem   default source = this taidaflow folder, build = build\w2-067-dbg
rem   e.g. the unfixed HEAD 2005c26 exported with "git archive" to build\w2-067-head:
rem        build-dbg.bat fresh build\w2-067-head build\w2-067-head\build
rem   (relative paths are relative to the taidaflow folder)
setlocal
set "ROOT=%~dp0..\..\..\.."
pushd "%ROOT%" || exit /b 1
set "SRC=."
set "BLD=build\w2-067-dbg"
if not "%~2"=="" set "SRC=%~2"
if not "%~3"=="" set "BLD=%~3"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || (popd & exit /b 1)
if /I "%~1"=="fresh" if exist "%BLD%" rmdir /s /q "%BLD%"
set "VSLANG=1033"
"C:\Qt\Tools\CMake_64\bin\cmake.exe" -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/qt.toolchain.cmake || (popd & exit /b 1)
"C:\Qt\Tools\CMake_64\bin\cmake.exe" --build "%BLD%" --target TaidaFlowApp || (popd & exit /b 1)
popd
exit /b 0
