@echo on
rem DEPLOY_AND_STARTUP.md 3.2 step 1b (literal; -LogFile added so the record goes to the w2-057 evidence folder)
set "REPO=D:\repo\codex\qmlTester"
powershell -ExecutionPolicy Bypass -File "%REPO%\taidaflow\scripts\safety_probe.ps1" -DeviceProfile simulator -Reason "manual" -LogFile "%REPO%\taidaflow\docs\evidence\w2-057\21-manual-test\safety-probe.log"
exit /b %ERRORLEVEL%
