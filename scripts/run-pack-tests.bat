@echo off
rem wasm-mirror pack native tests (package-integration §12.1): build the vendored pack as a
rem top-level project with WASM_MIRROR_BUILD_TESTS=ON into build\pack-tests (out of source,
rem the pack directory itself is not written) and run CTest (WasmMirror.Engine/Runtime).
rem Exit code 0 = all tests passed.
setlocal
set "ROOT=%~dp0.."
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set VSLANG=1033
pushd "%ROOT%" || exit /b 1
if exist "build\pack-tests" rmdir /s /q "build\pack-tests"
"%CMAKE%" -S integration-pack\wasm-mirror -B build\pack-tests -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe ^
    -DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/qt.toolchain.cmake ^
    -DWASM_MIRROR_BUILD_TESTS=ON || (popd & exit /b 1)
"%CMAKE%" --build build\pack-tests || (popd & exit /b 1)
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
"C:\Qt\Tools\CMake_64\bin\ctest.exe" --test-dir build\pack-tests --output-on-failure || (popd & exit /b 1)
popd
exit /b 0
