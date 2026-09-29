@echo off
rem w2-071 D2 evidence: same range queries through SqlManager of git 997042d (month loop) and of
rem the work tree (w2-071 month file listing), on the same data folders; outputs must be equal.
rem   1. git show 997042d:Core/SqlManager.{h,cpp} -> build\w2-071-compare\before-src (read-only git)
rem   2. build range_dump_before / range_dump_after / tst_sqlmanager_schema_once_before
rem   3. synthetic data (4 months, seeded by range_dump_after --seed) and a COPY of
rem      build\runtime-cwd\data\sensor_202609.sqlite (simulator data; the original is not touched)
rem   4. dump with both, compare SHA256; run the schema test against the old SqlManager (must fail)
rem Output: docs\evidence\w2-071\10-range-compare.txt (+ the dumps under build\w2-071-compare)
setlocal
set "TF=%~dp0..\..\..\..\.."
set "OUT=%TF%\build\w2-071-compare"
set "EV=%TF%\docs\evidence\w2-071"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%"
set "QT_FORCE_STDERR_LOGGING=1"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\before-src" "%OUT%\synthetic" "%OUT%\real" "%OUT%\cwd" || exit /b 1
pushd "%TF%" || exit /b 1
git show 997042d:Core/SqlManager.h > "%OUT%\before-src\SqlManager.h" || (popd & exit /b 1)
git show 997042d:Core/SqlManager.cpp > "%OUT%\before-src\SqlManager.cpp" || (popd & exit /b 1)
popd
C:\Qt\Tools\CMake_64\bin\cmake.exe -S "%~dp0." -B "%OUT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 "-DBEFORE_SRC=%OUT%\before-src" || exit /b 1
C:\Qt\Tools\CMake_64\bin\cmake.exe --build "%OUT%\build" || exit /b 1
copy /y "%TF%\build\runtime-cwd\data\sensor_202609.sqlite" "%OUT%\real\sensor_202609.sqlite" >nul || exit /b 1
rem Run in an empty folder: no data_schema.sql there (built-in schema, as in the app folder).
pushd "%OUT%\cwd"
"%OUT%\build\range_dump_after.exe" --seed "%OUT%\synthetic" || (popd & exit /b 1)
for %%D in (synthetic real) do (
    "%OUT%\build\range_dump_before.exe" --dump "%OUT%\%%D" "%OUT%\%%D-before.txt" || (popd & exit /b 1)
    "%OUT%\build\range_dump_after.exe" --dump "%OUT%\%%D" "%OUT%\%%D-after.txt" || (popd & exit /b 1)
)
popd
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$o='%OUT%'; $lines=@(); $fail=0;" ^
  "foreach ($d in 'synthetic','real') { $b=Get-FileHash \"$o\$d-before.txt\" -Algorithm SHA256; $a=Get-FileHash \"$o\$d-after.txt\" -Algorithm SHA256;" ^
  "  $n=(Get-Content \"$o\$d-after.txt\").Count; $rows=(Select-String -Path \"$o\$d-after.txt\" -Pattern ' items=([1-9][0-9]*) ' | Measure-Object).Count;" ^
  "  $same = $a.Hash -eq $b.Hash; if (-not $same) { $fail++ };" ^
  "  $lines += ('{0}: {1} query results ({2} with rows), before SHA256 {3}, after SHA256 {4} -> {5}' -f $d,$n,$rows,$b.Hash,$a.Hash,$(if ($same) {'IDENTICAL'} else {'DIFFERENT'})) };" ^
  "$lines += ('result: ' + $(if ($fail -eq 0) {'PASS (before == after)'} else {'FAIL'}));" ^
  "$lines | Set-Content -Encoding utf8 '%EV%\10-range-compare.txt'; $lines; exit $fail"
if errorlevel 1 exit /b 1
rem The Core/tests schema test against the OLD SqlManager: expected to fail (exit code != 0).
pushd "%OUT%\cwd"
"%OUT%\build\tst_sqlmanager_schema_once_before.exe" -o "%EV%\11-schema-test-against-997042d.txt,txt"
set "OLDRC=%ERRORLEVEL%"
popd
echo tst_sqlmanager_schema_once against SqlManager 997042d: exit code %OLDRC% (expected: not 0)>> "%EV%\11-schema-test-against-997042d.txt"
if "%OLDRC%"=="0" exit /b 1
exit /b 0
