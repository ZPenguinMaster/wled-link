<#
Builds the WLED Link version of WLED (official WLED 16.0.1 plus wledlink.patch) for the light's ESP32 and
puts it in wled\prebuilt\. Needs git, Node.js and PlatformIO (python -m pip install platformio).

  powershell -ExecutionPolicy Bypass -File wled\build.ps1

Then install it on the light, through the link:  python pc\wledlink.py wled-update
#>
$ErrorActionPreference = 'Stop'
$tag = 'v16.0.1'
$root = Split-Path $PSScriptRoot
$src = Join-Path $root 'wled-src'
$out = Join-Path $PSScriptRoot 'prebuilt\WLED_16.0.1_ESP32_wledlink.bin'

function Invoke-Checked([scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "failed: $cmd" }
}

if (-not (Test-Path $src)) {
    Invoke-Checked { git clone --depth 1 --branch $tag https://github.com/wled/WLED.git $src }
}
Push-Location $src
try {
    Invoke-Checked { git checkout -- . }
    Invoke-Checked { git apply (Join-Path $PSScriptRoot 'wledlink.patch') }
    # WLED builds with its own toolchain (a Tasmota build of the Arduino core); keep it apart from the bridge's.
    $env:PLATFORMIO_CORE_DIR = Join-Path $src '.platformio'
    Invoke-Checked { python -m platformio run -e esp32dev }
    Copy-Item 'build_output\release\WLED_16.0.1_ESP32.bin' $out -Force
    Write-Host "Built $out"
} finally {
    Pop-Location
}
