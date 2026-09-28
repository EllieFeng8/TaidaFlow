@echo off
rem w2-063 D2-03: docs\BUILD.md section 7 checks, run as documented against the manual build folders.
rem python is invoked as "py -3" (BUILD.md 2.5: on this PC "python" in cmd is the Microsoft Store alias).
rem Prints the exit code of every check; exit 0 only when every check returned 0.
cd /d "%~dp0..\..\..\.."
set FAIL=0
py -3 -B scripts\verify_pack.py
echo [w2-063] check verify_pack exit=%ERRORLEVEL%
if errorlevel 1 set FAIL=1
py -3 -B scripts\check_wasm_backend.py build\manual-desktop build\manual-wasm
echo [w2-063] check check_wasm_backend exit=%ERRORLEVEL%
if errorlevel 1 set FAIL=1
py -3 -B scripts\check_version_shadow.py build\manual-desktop build\manual-wasm
echo [w2-063] check check_version_shadow exit=%ERRORLEVEL%
if errorlevel 1 set FAIL=1
py -3 -B scripts\make_font_subset.py --check
echo [w2-063] check make_font_subset--check exit=%ERRORLEVEL%
if errorlevel 1 set FAIL=1
py -3 -B scripts\check_rest_routes.py
echo [w2-063] check check_rest_routes exit=%ERRORLEVEL%
if errorlevel 1 set FAIL=1
exit /b %FAIL%
