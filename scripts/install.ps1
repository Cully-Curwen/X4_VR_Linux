# One-time setup: fetch dependencies, build, and point X4's FreeTrack support at our DLL.
param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$gameRoot = Split-Path $projectRoot -Parent
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot 'X4.exe'))) {
    throw "X4.exe not found in '$gameRoot'. Put this repository inside the X4 Foundations install folder."
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is required (https://git-scm.com)' }

& (Join-Path $PSScriptRoot 'bootstrap.ps1')
& (Join-Path $PSScriptRoot 'build.ps1') -BuildDirectory build -SkipTests:$SkipTests

# X4 loads <Path>\FreeTrackClient64.dll when "OpenTrack Support" is on.
$key = 'HKCU:\Software\FreeTrack\FreeTrackClient'
$dllDirectory = Join-Path $projectRoot 'build\Release'
$previous = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).Path
if ($previous -and $previous -ne $dllDirectory) {
    # Keep the old value (e.g. a real FreeTrack/opentrack install) so uninstall.ps1 can restore it.
    Set-Content -LiteralPath (Join-Path $projectRoot 'config\freetrack-path.backup') -Value $previous -Encoding utf8
    Write-Warning "Replacing existing FreeTrack path '$previous' (backed up; uninstall.ps1 restores it)."
}
New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name Path -Value $dllDirectory
Write-Output "FreeTrack path set to $dllDirectory"

$captures = Join-Path $projectRoot 'reports\captures'
New-Item -ItemType Directory -Path $captures -Force | Out-Null
$stereo = Join-Path $captures 'stereo.txt'
if (-not (Test-Path -LiteralPath $stereo)) { Copy-Item (Join-Path $projectRoot 'config\stereo.txt') $stereo }
Write-Output 'Installed. Start SteamVR, then run scripts\play.ps1'
