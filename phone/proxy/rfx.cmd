@echo off
:: Shortcut for typing on the phone: "\rfx" in CMD Injector's terminal.
::   \rfx          refresh rfxproxy's boot entry (uninstall + install)
::   \rfx off      remove rfxproxy from boot
set RFX=C:\Data\Users\Public\Documents\retrofox
call "%RFX%\autostart-uninstall.cmd"
if /i "%1"=="off" goto :eof
call "%RFX%\autostart-install.cmd"
