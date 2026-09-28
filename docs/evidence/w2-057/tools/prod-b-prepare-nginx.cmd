@echo on
rem DEPLOY_AND_STARTUP.md 1.4 (b) - mkdir + copy literal; the Notepad replace step is done by the harness (same three replacements).
set "TF=D:\repo\codex\qmlTester\taidaflow\dist\TaidaFlow-20260928-c714aee"
set "DATA=D:\repo\codex\qmlTester\taidaflow\build\w2-057-manual-prod"
mkdir "%DATA%\nginx\conf" "%DATA%\nginx\logs" "%DATA%\nginx\temp" "%DATA%\exports"
copy "%TF%\deploy\nginx\taidaflow.conf" "%DATA%\nginx\conf\taidaflow.conf"
exit /b %ERRORLEVEL%
