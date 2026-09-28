@echo off
rem w2-063 emsdk real installation test (BUILD.md 2.4), NOT touching C:\tools\emsdk:
rem an emsdk git clone in build\em (made before this script, see report) is installed and activated
rem for 3.1.56 exactly as documented, without --permanent. Arg 1 = label printed in the log.
rem emsdk is called as .\emsdk.bat (this agent shell sets NoDefaultCurrentDirectoryInExePath=1, so cmd does not search the current folder).
rem Shows: HKCU\Environment EMSDK before/after (must stay absent: nothing permanent), emsdk list,
rem emcc --version, emscripten-version.txt, bundled node/python folders. Exit 0 only when all steps returned 0
rem and emcc reports 3.1.56.
cd /d "%~dp0..\..\..\..\build\em" || exit /b 1
echo [w2-063] %~1 cwd=%CD%
git log --oneline -1
rem Before the first "emsdk install" emsdk has no bundled Python and runs "python" from PATH. On this PC
rem "python" in cmd is the Microsoft Store alias, so put the real Python (found via the py launcher)
rem first on PATH - the workaround documented in BUILD.md 2.4.
for /f "delims=" %%p in ('py -3 -c "import sys,os;print(os.path.dirname(sys.executable))"') do set "PATH=%%p;%PATH%"
where python
reg query HKCU\Environment /v EMSDK >nul 2>&1 && (echo [w2-063] HKCU EMSDK before: SET) || (echo [w2-063] HKCU EMSDK before: not set)
call .\emsdk.bat install 3.1.56
echo [w2-063] step emsdk-install-3.1.56 exit=%ERRORLEVEL%
if errorlevel 1 exit /b 71
call .\emsdk.bat activate 3.1.56
echo [w2-063] step emsdk-activate-3.1.56 exit=%ERRORLEVEL%
if errorlevel 1 exit /b 72
reg query HKCU\Environment /v EMSDK >nul 2>&1 && (echo [w2-063] HKCU EMSDK after: SET) || (echo [w2-063] HKCU EMSDK after: not set)
call .\emsdk_env.bat
echo [w2-063] step emsdk_env exit=%ERRORLEVEL%
echo [w2-063] EMSDK=%EMSDK%
echo [w2-063] EMSDK_NODE=%EMSDK_NODE%
echo [w2-063] EMSDK_PYTHON=%EMSDK_PYTHON%
call emcc --version > "w2063_emcc.txt"
set RC=%ERRORLEVEL%
type "w2063_emcc.txt"
echo [w2-063] step emcc-version exit=%RC%
findstr /c:") 3.1.56 (" "w2063_emcc.txt" >nul
set RCV=%ERRORLEVEL%
del "w2063_emcc.txt"
echo [w2-063] emcc reports 3.1.56: %RCV% (0 = yes)
echo [w2-063] upstream\emscripten\emscripten-version.txt:
type upstream\emscripten\emscripten-version.txt
echo [w2-063] node\ and python\ folders:
dir /b node python
call .\emsdk.bat list > "w2063_list.txt"
echo [w2-063] step emsdk-list exit=%ERRORLEVEL%
findstr /i "3.1.56 INSTALLED ACTIVE" "w2063_list.txt"
del "w2063_list.txt"
if not "%RC%"=="0" exit /b 73
if not "%RCV%"=="0" exit /b 74
exit /b 0
