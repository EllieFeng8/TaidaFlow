@echo off
rem w2-063 fresh-clone step 5 (BUILD.md 2.6 / 9): the build scripts stop with a message and exit 1
rem when a tool is not where the variables say. No build folder is touched (the checks run first).
rem Exit 0 only when every case returned exit 1.
cd /d "%~dp0..\..\..\..\build\fc" || exit /b 1
set FAIL=0
cmd /c "set TAIDAFLOW_QT_ROOT=C:\no-such-dir\Qt\6.8.3&& scripts\build-desktop.bat"
echo [w2-063] case desktop bad TAIDAFLOW_QT_ROOT exit=%ERRORLEVEL% (expected 1)
if not "%ERRORLEVEL%"=="1" set FAIL=1
cmd /c "set TAIDAFLOW_QT_TOOLS=C:\no-such-dir\Tools&& scripts\build-desktop.bat"
echo [w2-063] case desktop bad TAIDAFLOW_QT_TOOLS exit=%ERRORLEVEL% (expected 1)
if not "%ERRORLEVEL%"=="1" set FAIL=1
cmd /c "set TAIDAFLOW_VCVARS64=C:\no-such-dir\vcvars64.bat&& scripts\build-desktop.bat"
echo [w2-063] case desktop bad TAIDAFLOW_VCVARS64 exit=%ERRORLEVEL% (expected 1)
if not "%ERRORLEVEL%"=="1" set FAIL=1
cmd /c "set TAIDAFLOW_EMSDK=C:\no-such-dir\emsdk&& scripts\build-wasm.bat"
echo [w2-063] case wasm bad TAIDAFLOW_EMSDK exit=%ERRORLEVEL% (expected 1)
if not "%ERRORLEVEL%"=="1" set FAIL=1
cmd /c "set TAIDAFLOW_QT_ROOT=C:\no-such-dir\Qt\6.8.3&& scripts\build-wasm.bat"
echo [w2-063] case wasm bad TAIDAFLOW_QT_ROOT exit=%ERRORLEVEL% (expected 1)
if not "%ERRORLEVEL%"=="1" set FAIL=1
exit /b %FAIL%
