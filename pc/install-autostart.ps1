<#
Starts WLED Link automatically when you log in to Windows, in the background (no console window).

  powershell -ExecutionPolicy Bypass -File install-autostart.ps1              # install and start now
  powershell -ExecutionPolicy Bypass -File install-autostart.ps1 -Uninstall   # remove and stop
#>
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$shortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'WLED Link.lnk'
$script = Join-Path $PSScriptRoot 'wledlink.py'

function Get-WledLinkProcess {
    Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe'" |
        Where-Object { $_.CommandLine -like '*wledlink.py*' -and $_.CommandLine -notmatch 'wledlink\.py"?\s+(status|ports|white|signalrgb|off|bridge-config|wled-wifi|flash-bridge)' }
}

if ($Uninstall) {
    Remove-Item $shortcut -ErrorAction SilentlyContinue
    Get-WledLinkProcess | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    Write-Host 'Removed the WLED Link autostart and stopped the background copy.'
    return
}

$python = (Get-Command python -ErrorAction Stop).Source
$pythonw = Join-Path (Split-Path $python) 'pythonw.exe'
if (-not (Test-Path $pythonw)) { throw "pythonw.exe not found next to $python" }
& $python -c 'import serial' 2>$null
if ($LASTEXITCODE -ne 0) { & $python -m pip install pyserial }

$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = $pythonw
$link.Arguments = "`"$script`" run"
$link.WorkingDirectory = $PSScriptRoot
$link.Description = 'WLED Link: WLED through the USB bridge ESP32'
$link.Save()

if (-not (Get-WledLinkProcess)) {
    Start-Process -FilePath $pythonw -ArgumentList "`"$script`" run" -WorkingDirectory $PSScriptRoot
}
Write-Host "Installed $shortcut"
Write-Host 'WLED Link is running in the background. Status page: http://127.0.0.2/__wledlink'
Write-Host "Log file: $env:LOCALAPPDATA\wledlink\wledlink.log"
