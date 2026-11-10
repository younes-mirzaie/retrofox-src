@echo off
:: Double-click on the PC to refresh rfxproxy's boot entry on the Lumia (over Wi-Fi, via
:: CMD Injector's telnet): removes the old line from Startup.bat and adds the current one.
:: The phone must be on and on the same Wi-Fi. Undo with remove-autostart.cmd.
echo Updating rfxproxy's boot entry on the phone...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0phone-cmd.ps1" -IdleMs 5000 ^
  "C:\Data\Users\Public\Documents\retrofox\autostart-uninstall.cmd" ^
  "C:\Data\Users\Public\Documents\retrofox\autostart-install.cmd" ^
  "findstr /c:rfxproxy C:\Windows\System32\Startup.bat"
echo.
echo Done. The new entry takes effect at the next reboot.
pause
