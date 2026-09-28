@echo off
rem w2-065: builds and runs App/tests (tst_appconfig, tst_runtimeinfo, tst_applog) with CTest into build\w2-065-app-tests.
rem Usage (repo folder): docs\evidence\w2-065\tools\run-app-tests.bat      Exit code = the one of ctest (0 = all passed).
setlocal
set "ROOT=%~dp0..\..\..\.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
pushd "%ROOT%" || exit /b 1
if exist "build\w2-065-app-tests" rmdir /s /q "build\w2-065-app-tests"
C:\Qt\Tools\CMake_64\bin\cmake.exe -S App\tests -B build\w2-065-app-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 || (popd & exit /b 1)
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\w2-065-app-tests || (popd & exit /b 1)
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir build\w2-065-app-tests --output-on-failure -V
set "RC=%ERRORLEVEL%"
popd
echo ctest exit code %RC%
exit /b %RC%
