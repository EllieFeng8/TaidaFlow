@echo off
rem Build the UNMODIFIED desktop app of a given commit (default dc91f01, core branch HEAD
rem before the WASM v4 integration) as the pixel-regression baseline.
rem   scripts\build-baseline.bat [commit]
rem Source is exported with `git archive` into build\baseline-src (the work tree is not
rem touched); binary goes to build\baseline-desktop. Exit code 0 = success.
setlocal
set "ROOT=%~dp0.."
set "COMMIT=%~1"
if "%COMMIT%"=="" set "COMMIT=dc91f01"
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set VSLANG=1033

pushd "%ROOT%" || exit /b 1
if exist "build\baseline-src" rmdir /s /q "build\baseline-src"
if exist "build\baseline-desktop" rmdir /s /q "build\baseline-desktop"
mkdir "build\baseline-src" || (popd & exit /b 1)
git archive --format=tar -o "build\baseline-%COMMIT%.tar" %COMMIT% || (popd & exit /b 1)
tar -xf "build\baseline-%COMMIT%.tar" -C "build\baseline-src" || (popd & exit /b 1)
"%CMAKE%" -S "build\baseline-src" -B "build\baseline-desktop" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe ^
    -DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/qt.toolchain.cmake || (popd & exit /b 1)
"%CMAKE%" --build "build\baseline-desktop" --target TaidaFlowApp || (popd & exit /b 1)
popd
exit /b 0
