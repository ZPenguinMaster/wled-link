@echo off
rem Double-click to install WLED Link: Start menu, tray icon, starts with Windows. Settings > Apps removes it
rem again (or run "install.cmd -Uninstall").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
if errorlevel 1 pause
