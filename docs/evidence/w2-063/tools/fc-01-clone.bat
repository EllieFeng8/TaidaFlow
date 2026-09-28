@echo off
rem w2-063 fresh-clone simulation step 1 (BUILD.md 1.1), in build\ (a different repo location):
rem  a) git clone -b core <GitHub> fc2  -> must be on branch core; fc2 is deleted again
rem  b) git clone <GitHub> fc, then "git checkout core" (expected to FAIL on Windows: ambiguous with
rem     the folder Core) and "git switch core" (expected 0)
rem  c) bring fc to this round's state: fetch the local core HEAD (2 commits not yet pushed),
rem     detach there, copy the files changed in this round (uncommitted).
rem Exit 0 only when every step behaved as expected.
set "TF=%~dp0..\..\..\.."
cd /d "%TF%\build" || exit /b 1
if exist fc (echo [w2-063] build\fc already exists - remove it first & exit /b 2)
if exist fc2 (echo [w2-063] build\fc2 already exists - remove it first & exit /b 2)
git clone -b core https://github.com/EllieFeng8/TaidaFlow.git fc2
echo [w2-063] step a git-clone-b-core exit=%ERRORLEVEL%
if errorlevel 1 exit /b 10
git -C fc2 status --short --branch
rmdir /s /q fc2
git clone https://github.com/EllieFeng8/TaidaFlow.git fc
echo [w2-063] step b git-clone exit=%ERRORLEVEL%
if errorlevel 1 exit /b 11
cd fc
git checkout core
echo [w2-063] step b git-checkout-core exit=%ERRORLEVEL% (expected non-zero: ambiguous with folder Core)
if not errorlevel 1 exit /b 12
git switch core
echo [w2-063] step b git-switch-core exit=%ERRORLEVEL%
if errorlevel 1 exit /b 13
git status --short --branch
git log --oneline -1
git fetch "%TF%" core:refs/remotes/local/core
echo [w2-063] step c fetch-local-head exit=%ERRORLEVEL%
if errorlevel 1 exit /b 14
git checkout --detach local/core
echo [w2-063] step c checkout-local-head exit=%ERRORLEVEL%
if errorlevel 1 exit /b 15
git log --oneline -1
copy /y "%TF%\scripts\build-desktop.bat" scripts\build-desktop.bat
copy /y "%TF%\scripts\build-wasm.bat" scripts\build-wasm.bat
copy /y "%TF%\docs\BUILD.md" docs\BUILD.md
echo [w2-063] step c copy-round-files exit=%ERRORLEVEL%
git status --short
cd
exit /b 0
