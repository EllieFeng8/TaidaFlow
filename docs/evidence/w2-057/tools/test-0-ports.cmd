@echo on
rem DEPLOY_AND_STARTUP.md 3.2 step 0 (literal)
netstat -ano | findstr LISTENING | findstr /C:":80 " /C:":502 " /C:":8124 " /C:":8125 " /C:":18125 "
echo findstr exit %ERRORLEVEL% (1 = nothing listening)
exit /b 0
