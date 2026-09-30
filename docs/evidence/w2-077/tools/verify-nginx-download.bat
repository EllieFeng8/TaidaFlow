@echo off
rem w2-077 D1: re-run the docs\BUILD.md 2.9 nginx steps 2 and 4 on the reference PC's downloaded files
rem (C:\tools\nginx\_download, read only): the zip, its .asc and nginx.org's pluknet.key are copied to
rem build\w2-077-nginx-verify; gpg (Git for Windows) with a keyring in that folder; certutil SHA-256;
rem nginx -v of C:\tools\nginx\nginx-1.30.5. Nothing outside build\ is written.
rem Usage: verify-nginx-download.bat     Exit 0 = Good signature + SHA-256 match + nginx/1.30.5.
setlocal
for %%I in ("%~dp0..\..\..\..") do set "SRC=%%~fI"
set "W=%SRC%\build\w2-077-nginx-verify"
set "GPG=C:\Program Files\Git\usr\bin\gpg.exe"
if exist "%W%" rmdir /s /q "%W%"
mkdir "%W%" || exit /b 2
copy /y C:\tools\nginx\_download\nginx-1.30.5.zip "%W%\" >nul || exit /b 2
copy /y C:\tools\nginx\_download\nginx-1.30.5.zip.asc "%W%\" >nul || exit /b 2
copy /y C:\tools\nginx\_download\keys\pluknet.key "%W%\" >nul || exit /b 2
cd /d "%W%"
mkdir gnupg
set GNUPGHOME=gnupg
"%GPG%" --version | findstr /b "gpg"
"%GPG%" --import pluknet.key
echo import exit=%ERRORLEVEL%
"%GPG%" --status-fd 1 --verify nginx-1.30.5.zip.asc nginx-1.30.5.zip > verify.txt 2>&1
set VRC=%ERRORLEVEL%
type verify.txt
echo verify exit=%VRC%
"C:\Program Files\Git\usr\bin\gpgconf.exe" --kill gpg-agent >nul 2>&1
certutil -hashfile nginx-1.30.5.zip SHA256 > sha.txt
type sha.txt
C:\tools\nginx\nginx-1.30.5\nginx.exe -v > ver.txt 2>&1
type ver.txt
set FAIL=0
if not "%VRC%"=="0" set FAIL=1
findstr /c:"GOODSIG C8464D549AF75C0A" verify.txt >nul || set FAIL=1
findstr /c:"VALIDSIG D6786CE303D9A9022998DC6CC8464D549AF75C0A" verify.txt >nul || set FAIL=1
findstr /x "e5afe28b6a50bec92c478bfe1a4d3758206b80fb77159277bc5c4e88955c2a35" sha.txt >nul || set FAIL=1
findstr /c:"nginx version: nginx/1.30.5" ver.txt >nul || set FAIL=1
if not exist C:\tools\nginx\nginx-1.30.5\conf\mime.types set FAIL=1
echo verify-nginx-download: FAIL=%FAIL%
exit /b %FAIL%
