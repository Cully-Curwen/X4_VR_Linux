# Launch X4 in VR: Vulkan layer (OpenVR presentation) + FreeTrack pose source.
param([ValidateSet('build','build-next')][string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$expected = Join-Path $projectRoot "$BuildDirectory\Release"
$current = (Get-ItemProperty -Path 'HKCU:\Software\FreeTrack\FreeTrackClient' -ErrorAction SilentlyContinue).Path
if ($current -ne $expected) {
    Write-Warning "FreeTrack path is '$current', expected '$expected'. Run scripts\install.ps1 or head tracking will not work."
}
if (-not (Get-Process vrserver -ErrorAction SilentlyContinue)) {
    Write-Warning 'SteamVR does not seem to be running. Start it (and your headset software) first.'
}
$captures = Join-Path $projectRoot 'reports\captures'
New-Item -ItemType Directory -Path $captures -Force | Out-Null
$stereo = Join-Path $captures 'stereo.txt'
if (-not (Test-Path -LiteralPath $stereo)) { Copy-Item (Join-Path $projectRoot 'config\stereo.txt') $stereo }
& (Join-Path $PSScriptRoot 'observe.ps1') -Target Game -OpenVRBootstrap -CrashWatch `
    -BuildDirectory $BuildDirectory -GameArgs '-skipintro -nocputhrottle'
