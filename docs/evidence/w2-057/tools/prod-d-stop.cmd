@echo on
rem DEPLOY_AND_STARTUP.md 1.4 (c) line 4 (nginx -s quit) and 1.4 (d) (taskkill without /F).
set "TF=D:\repo\codex\qmlTester\taidaflow\dist\TaidaFlow-20260928-c714aee"
"%TF%\nginx\nginx.exe" -p "D:/repo/codex/qmlTester/taidaflow/build/w2-057-manual-prod/nginx/" -c conf/taidaflow.conf -s quit
echo nginx -s quit exit %ERRORLEVEL%
taskkill /IM TaidaFlowApp.exe
echo taskkill exit %ERRORLEVEL%
exit /b 0
