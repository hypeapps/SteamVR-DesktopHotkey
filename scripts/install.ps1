# Registers the desktop_hotkey driver with SteamVR (vrpathreg adddriver).
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$driverDir = Join-Path $root 'desktop_hotkey'
if (-not (Test-Path (Join-Path $driverDir 'driver.vrdrivermanifest'))) {
    throw "Driver folder not found: $driverDir"
}

if (Get-Process -Name vrserver, vrmonitor -ErrorAction SilentlyContinue) {
    throw 'Please close SteamVR first.'
}

$vrpathFile = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
if (-not (Test-Path $vrpathFile)) {
    throw "SteamVR not found ($vrpathFile is missing). Start SteamVR once, close it and try again."
}
$vrpaths = Get-Content $vrpathFile -Raw | ConvertFrom-Json
$vrpathreg = $vrpaths.runtime |
    ForEach-Object { Join-Path $_ 'bin\win64\vrpathreg.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $vrpathreg) {
    throw 'vrpathreg.exe not found in the SteamVR runtime folder.'
}

Write-Host "SteamVR tool: $vrpathreg"
& $vrpathreg removedriverswithname desktop_hotkey | Out-Null
& $vrpathreg adddriver $driverDir
Write-Host ''
Write-Host "Installed driver from: $driverDir"
Write-Host 'Do not move this folder. Start SteamVR and check Settings > Startup/Shutdown > Manage Add-ons.'
