@echo off
rem Double-click to make WLED Link start at every login. Run "install-autostart.cmd -Uninstall" to remove it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-autostart.ps1" %*
echo.
pause
