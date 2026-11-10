@echo off
:: Removes the rfxproxy lines that autostart-install.cmd added to CMD Injector's Startup.bat.
set STARTUP=C:\Windows\System32\Startup.bat
set RFX=C:\Data\Users\Public\Documents\retrofox

findstr /v /c:"retrofox rfxproxy" "%STARTUP%" | findstr /v /c:"%RFX%\rfxproxy.exe" > "%RFX%\Startup.bat.tmp"
copy /y "%RFX%\Startup.bat.tmp" "%STARTUP%" >nul
del "%RFX%\Startup.bat.tmp"
echo Removed rfxproxy from %STARTUP%
