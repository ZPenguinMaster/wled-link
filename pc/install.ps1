<#
Installs WLED Link for this Windows user (no administrator rights needed):
  - the Python packages it uses,
  - "start with Windows": a scheduled task that starts it at sign-in, on wake and unlock, and checks every minute
    that it's running (unless you chose Quit, until you sign in again or open it),
  - WLED Link in the Start menu, and in Settings > Apps (with an Uninstall button),
then starts it and opens it.

  install.cmd                                                            # double-click
  powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall        # remove it (settings are kept)
#>
param([switch]$Uninstall, [switch]$NoOpen)

$ErrorActionPreference = 'Stop'
$appName = 'WLED Link'
$taskName = 'WLED Link'
$appId = 'ZPenguinMaster.WLEDLink'  # the same id the app window uses, so the taskbar files them together
$here = $PSScriptRoot
$script = Join-Path $here 'wledlink.py'
$icon = Join-Path $here 'app\icon.ico'
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) "$appName.lnk"
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\WLEDLink'
$oldShortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'WLED Link.lnk'  # what early versions installed

function Get-WledLinkProcess {
    Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe'" |
        Where-Object { $_.CommandLine -match 'wledlink\.py"?\s+(run|app)' -or $_.CommandLine -match 'wledlink\.py"?\s*$' }
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
    Stop-WledLink
    Remove-Item $startMenu, $oldShortcut -ErrorAction SilentlyContinue
    Remove-Item $uninstallKey -Recurse -ErrorAction SilentlyContinue
    Write-Host "$appName was removed. Its settings stay in $env:LOCALAPPDATA\wledlink, for a later install."
    return
}

# ---- Python and the packages
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw 'Python 3 is needed: install it from python.org (tick "Add python.exe to PATH"), then run this again.' }
$pythonw = Join-Path (Split-Path $python) 'pythonw.exe'
if (-not (Test-Path $pythonw)) { throw "pythonw.exe not found next to $python" }
Write-Host 'Installing the Python packages WLED Link uses...'
& $python -m pip install --disable-pip-version-check -q -r (Join-Path $here 'requirements.txt')
if ($LASTEXITCODE -ne 0) { throw 'pip could not install the packages (see above).' }

# ---- start with Windows
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$esc = { param($s) [System.Security.SecurityElement]::Escape($s) }
$start = (Get-Date).AddMinutes(1).ToString('yyyy-MM-ddTHH:mm:ss')
# Priority 4 = normal: tasks default to 7 (below normal, low I/O priority), which would let a busy game delay
# the lights. No time limit (the default stops a task after 3 days), and it keeps running on battery.
$xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Starts WLED Link with Windows: at sign-in, on wake and unlock, and every minute if it isn't running (unless Quit was chosen).</Description>
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
      <Arguments>"$(& $esc $script)" run --background</Arguments>
      <WorkingDirectory>$(& $esc $here)</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"@
Register-ScheduledTask -TaskName $taskName -Xml $xml -Force | Out-Null
Remove-Item $oldShortcut -ErrorAction SilentlyContinue

# ---- Start menu entry, filed under the app's own taskbar id
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($startMenu)
$lnk.TargetPath = $pythonw
$lnk.Arguments = "`"$script`" app"
$lnk.WorkingDirectory = $here
$lnk.IconLocation = "$icon,0"
$lnk.Description = 'Control your WLED light, and the link that reaches it'
$lnk.Save()
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public static class WledLinkShortcut {
    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        void GetCount(out uint count);
        void GetAt(uint index, out PropKey key);
        void GetValue(ref PropKey key, IntPtr value);
        void SetValue(ref PropKey key, ref PropVariant value);
        void Commit();
    }
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct PropKey { public Guid FormatId; public uint PropertyId; }
    [StructLayout(LayoutKind.Explicit)]
    public struct PropVariant { [FieldOffset(0)] public ushort Type; [FieldOffset(8)] public IntPtr Pointer; }
    [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
    class ShellLink { }
    public static void SetAppId(string path, string appId) {
        var link = new ShellLink();
        ((IPersistFile)link).Load(path, 2);
        var key = new PropKey { FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), PropertyId = 5 };
        var value = new PropVariant { Type = 31, Pointer = Marshal.StringToCoTaskMemUni(appId) };
        try {
            var store = (IPropertyStore)link;
            store.SetValue(ref key, ref value);
            store.Commit();
            ((IPersistFile)link).Save(path, true);
        } finally {
            Marshal.FreeCoTaskMem(value.Pointer);
        }
    }
}
'@
try { [WledLinkShortcut]::SetAppId($startMenu, $appId) } catch { Write-Warning "The Start menu entry works, but its taskbar grouping couldn't be set: $_" }

# ---- Settings > Apps
$version = (Select-String -Path $script -Pattern '^VERSION = "([^"]+)"').Matches[0].Groups[1].Value
New-Item -Path $uninstallKey -Force | Out-Null
$uninstallCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$(Join-Path $here 'install.ps1')`" -Uninstall"
@{ DisplayName = $appName; DisplayVersion = $version; Publisher = 'WLED Link'; DisplayIcon = $icon;
   InstallLocation = (Split-Path $here); UninstallString = $uninstallCmd; QuietUninstallString = $uninstallCmd;
   URLInfoAbout = 'https://github.com/ZPenguinMaster/wled-link' }.GetEnumerator() | ForEach-Object {
    Set-ItemProperty -Path $uninstallKey -Name $_.Key -Value $_.Value
}
Set-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -Type DWord

# ---- hand the running copy over to the task (so the watchdog sees it as its own), then open the app
Stop-WledLink
Remove-Item (Join-Path $env:LOCALAPPDATA 'wledlink\quit') -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Start-ScheduledTask -TaskName $taskName
Write-Host "$appName $version is installed: in the Start menu and next to the clock, and it starts with Windows."
if (-not $NoOpen) { Start-Process -FilePath $pythonw -ArgumentList "`"$script`" app" -WorkingDirectory $here }
