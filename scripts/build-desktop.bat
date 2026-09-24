@echo off
rem TaidaFlow desktop (MSVC 2022 x64, Qt 6.8.3) configure + build via CMakePresets "desktop-release".
rem Usage: scripts\build-desktop.bat          -> build\desktop (incremental)
rem        scripts\build-desktop.bat fresh    -> wipe build\desktop first
rem Exit code = cmake exit code (0 = success).
setlocal
set "ROOT=%~dp0.."
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

pushd "%ROOT%" || exit /b 1
if /I "%~1"=="fresh" if exist "build\desktop" rmdir /s /q "build\desktop"
"%CMAKE%" --preset desktop-release || (popd & exit /b 1)
"%CMAKE%" --build --preset desktop-release || (popd & exit /b 1)
popd
exit /b 0
