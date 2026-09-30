@echo off
rem w2-084: tests of the WebAssembly loading page's automatic reload (App\wasm\TaidaFlowApp.shell.html)
rem with node, without a browser. Uses the node.exe of emsdk (read-only; no npm package needed).
rem Usage: App\wasm\tests\run-shell-tests.bat [path of a built TaidaFlowApp.html]
rem   with a path: also checks the configured page of that wasm build (e.g. build\wasm-release\TaidaFlowApp.html).
rem Exit code 0 = all tests passed.
setlocal
set "NODE=C:\tools\emsdk\node\16.20.0_64bit\bin\node.exe"
if not exist "%NODE%" (
    echo node not found: %NODE%
    exit /b 9
)
if "%~1"=="" (
    "%NODE%" "%~dp0shell-autoreload.test.js"
) else (
    "%NODE%" "%~dp0shell-autoreload.test.js" --page "%~f1"
)
exit /b %ERRORLEVEL%
