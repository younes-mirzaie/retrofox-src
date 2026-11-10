@echo off
:: Starts rfxproxy at every boot by appending one line to CMD Injector's Startup.bat
:: (CMD Injector runs C:\Windows\System32\Startup.bat when the phone boots).
:: Safe to run twice; undo with autostart-uninstall.cmd.
set STARTUP=C:\Windows\System32\Startup.bat
set RFX=C:\Data\Users\Public\Documents\retrofox

findstr /c:"retrofox rfxproxy" "%STARTUP%" >nul 2>&1 && (
    echo rfxproxy is already in %STARTUP%
    goto :eof
)
copy /y "%STARTUP%" "%RFX%\Startup.bat.before-rfxproxy" >nul
>>"%STARTUP%" echo.
>>"%STARTUP%" echo REM retrofox rfxproxy: HTTPS fixer for IE on port 8080 (this phone only)
>>"%STARTUP%" echo start /b "" "%RFX%\rfxproxy.exe" 8080 0.0.0.0 ^> "%RFX%\proxy.log" 2^>^&1
echo Added rfxproxy to %STARTUP% (backup: %RFX%\Startup.bat.before-rfxproxy)
