@echo off
:: Double-click on the PC to remove rfxproxy from the Lumia's boot (Startup.bat), over Wi-Fi.
echo Removing rfxproxy from the phone's boot...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0phone-cmd.ps1" -IdleMs 5000 ^
  "C:\Data\Users\Public\Documents\retrofox\autostart-uninstall.cmd"
echo.
pause
