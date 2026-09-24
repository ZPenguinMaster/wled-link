<#
Keeps WLED Link running in the background (no console window): it starts when you log in, when the
PC wakes up or is unlocked, and a watchdog checks every minute and starts it again if it isn't running
(for example if it was closed or crashed). Uses a Windows scheduled task named "WLED Link".

  powershell -ExecutionPolicy Bypass -File install-autostart.ps1              # install and start now
  powershell -ExecutionPolicy Bypass -File install-autostart.ps1 -Uninstall   # remove and stop
#>
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$taskName = 'WLED Link'
$oldShortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'WLED Link.lnk'  # what older versions installed
$script = Join-Path $PSScriptRoot 'wledlink.py'

function Get-WledLinkProcess {
    Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe'" |
        Where-Object { $_.CommandLine -match 'wledlink\.py"?\s+run' -or $_.CommandLine -match 'wledlink\.py"?\s*$' }
}

function Stop-WledLink {
    if (-not (Get-WledLinkProcess)) { return }
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) { & $py.Source $script stop *> $null }  # a clean stop first
    for ($i = 0; $i -lt 30 -and (Get-WledLinkProcess); $i++) { Start-Sleep -Milliseconds 100 }
    Get-WledLinkProcess | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $oldShortcut -ErrorAction SilentlyContinue
    Stop-WledLink
    Write-Host 'Removed the WLED Link autostart and stopped the background copy.'
    return
}

$python = (Get-Command python -ErrorAction Stop).Source
$pythonw = Join-Path (Split-Path $python) 'pythonw.exe'
if (-not (Test-Path $pythonw)) { throw "pythonw.exe not found next to $python" }
& $python -c 'import serial' 2>$null
if ($LASTEXITCODE -ne 0) { & $python -m pip install pyserial }

$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$esc = { param($s) [System.Security.SecurityElement]::Escape($s) }
$start = (Get-Date).AddMinutes(1).ToString('yyyy-MM-ddTHH:mm:ss')
# Priority 4 = normal. Tasks default to 7 (below normal, low I/O priority), which would let a busy
# game delay the lights. No time limit (the default stops a task after 3 days), and it keeps running
# on battery, which laptops otherwise refuse.
$xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Keeps WLED Link running: reaches WLED through the USB bridge ESP32. Starts at log-in, on wake and unlock, and every minute if it isn't running.</Description>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger><Enabled>true</Enabled><UserId>$(& $esc $user)</UserId></LogonTrigger>
    <SessionStateChangeTrigger><Enabled>true</Enabled><StateChange>SessionUnlock</StateChange><UserId>$(& $esc $user)</UserId></SessionStateChangeTrigger>
    <EventTrigger>
      <Enabled>true</Enabled>
      <Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="System"&gt;&lt;Select Path="System"&gt;*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription>
    </EventTrigger>
    <TimeTrigger>
      <Enabled>true</Enabled>
      <StartBoundary>$start</StartBoundary>
      <Repetition><Interval>PT1M</Interval><StopAtDurationEnd>false</StopAtDurationEnd></Repetition>
    </TimeTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author"><UserId>$(& $esc $user)</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>4</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>$(& $esc $pythonw)</Command>
      <Arguments>"$(& $esc $script)" run</Arguments>
      <WorkingDirectory>$(& $esc $PSScriptRoot)</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"@
Register-ScheduledTask -TaskName $taskName -Xml $xml -Force | Out-Null
Remove-Item $oldShortcut -ErrorAction SilentlyContinue

# Hand the running copy over to the task, so the watchdog sees it as its own and stays idle.
Stop-WledLink
Start-Sleep -Milliseconds 500
Start-ScheduledTask -TaskName $taskName
Write-Host "Installed the '$taskName' task: WLED Link starts at log-in, on wake and unlock, and within a minute if it ever stops."
Write-Host 'WLED Link is running in the background. Control page: http://127.0.0.2/__wledlink'
Write-Host "Log file: $env:LOCALAPPDATA\wledlink\wledlink.log"
