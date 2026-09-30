@echo off
setlocal
rem usage: run-clean-path.bat <package folder> <output config.json (outside the package)>
rem w2-074: TaidaFlowApp.exe of the package with a PATH of Windows system folders only (no Qt / VS / emsdk)
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set QtMsBuild=
set QT_PLUGIN_PATH=
set QML2_IMPORT_PATH=
set QML_IMPORT_PATH=
set TAIDAFLOW_CONFIG=
set TAIDAFLOW_DEVICE_PROFILE=
echo PATH=%PATH%
echo --- where Qt6Core.dll (expect: not found on PATH)
where Qt6Core.dll 2>&1
echo --- where vcruntime140_1.dll / msvcp140_2.dll on PATH (System32 copies are allowed but the package has its own)
where msvcp140_2.dll 2>&1
set "OUT=%~2"
if exist "%OUT%" del "%OUT%"
"%~1\TaidaFlowApp.exe" --write-default-config "%OUT%"
echo TaidaFlowApp.exe exit=%errorlevel%
if exist "%OUT%" (echo written: %OUT%) else (echo NOT written)
endlocal
