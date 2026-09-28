@echo off
rem w2-062 D8: DEPLOY_AND_STARTUP.md section 5.1 typed as written (only SRC / OUT / CRT version filled in),
rem output below build\ of this repo. Usage: docs\evidence\w2-062\tools\manual-deploy-5-1.bat
setlocal
set "SRC=%~dp0..\..\..\.."
for %%i in ("%SRC%") do set "SRC=%%~fi"
set "OUT=%SRC%\build\w2-062-manual\TaidaFlow"
set "QT=C:\Qt\6.8.3\msvc2022_64"
if exist "%SRC%\build\w2-062-manual" rmdir /s /q "%SRC%\build\w2-062-manual"
mkdir "%OUT%" || exit /b 1
copy "%SRC%\build\desktop\TaidaFlowApp.exe" "%OUT%\" || exit /b 1
set "PATH=%QT%\bin;%PATH%"
"%QT%\bin\windeployqt.exe" --release --no-compiler-runtime --no-translations --skip-plugin-types qmltooling,canbus --exclude-plugins qsqlmimer,qsqlodbc,qsqlpsql --qmldir "%SRC%\TaidaFlowContent" --qmldir "%SRC%\TaidaFlow" --qmldir "%SRC%\Dependencies" "%OUT%\TaidaFlowApp.exe" || exit /b 1
copy "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT\*.dll" "%OUT%\" || exit /b 1
mkdir "%OUT%\web"
for %%f in (TaidaFlowApp.html TaidaFlowApp.js TaidaFlowApp.wasm qtloader.js qtlogo.svg) do copy "%SRC%\build\wasm-release\%%f" "%OUT%\web\" || exit /b 1
mkdir "%OUT%\nginx\conf" "%OUT%\nginx\logs" "%OUT%\nginx\temp"
copy "C:\tools\nginx\nginx-1.30.5\nginx.exe" "%OUT%\nginx\" || exit /b 1
xcopy /e /i "C:\tools\nginx\nginx-1.30.5\docs" "%OUT%\nginx\docs" || exit /b 1
copy "C:\tools\nginx\nginx-1.30.5\conf\mime.types" "%OUT%\nginx\conf\" || exit /b 1
echo manual 5.1 done: %OUT%
exit /b 0
