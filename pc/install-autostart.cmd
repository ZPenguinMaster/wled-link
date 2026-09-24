@echo off
rem Double-click to keep WLED Link running (starts at log-in, on wake, and again within a minute if it
rem ever stops). Run "install-autostart.cmd -Uninstall" to remove it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-autostart.ps1" %*
echo.
pause
