# Unregisters the desktop_hotkey driver from SteamVR.
$ErrorActionPreference = 'Stop'

if (Get-Process -Name vrserver, vrmonitor -ErrorAction SilentlyContinue) {
    throw 'Please close SteamVR first.'
}

$vrpathFile = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
$vrpaths = Get-Content $vrpathFile -Raw | ConvertFrom-Json
$vrpathreg = $vrpaths.runtime |
    ForEach-Object { Join-Path $_ 'bin\win64\vrpathreg.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $vrpathreg) {
    throw 'vrpathreg.exe not found in the SteamVR runtime folder.'
}

& $vrpathreg removedriverswithname desktop_hotkey
Write-Host 'Driver desktop_hotkey removed from SteamVR.'
