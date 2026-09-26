# Undo install.ps1: restore (or remove) the FreeTrack registry path. Game files were never modified.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$key = 'HKCU:\Software\FreeTrack\FreeTrackClient'
$backup = Join-Path $projectRoot 'config\freetrack-path.backup'
if (Test-Path -LiteralPath $backup) {
    $previous = (Get-Content -LiteralPath $backup -Raw).Trim()
    Set-ItemProperty -Path $key -Name Path -Value $previous
    Remove-Item -LiteralPath $backup
    Write-Output "Restored previous FreeTrack path: $previous"
} else {
    Remove-Item -Path $key -Recurse -ErrorAction SilentlyContinue
    $parent = 'HKCU:\Software\FreeTrack'
    if ((Test-Path $parent) -and -not (Get-ChildItem $parent)) { Remove-Item -Path $parent }
    Write-Output 'Removed FreeTrack registry path.'
}
# The launcher's HUD distance extension (generated, marked by x4vr_hud.txt).
$hud = Join-Path (Split-Path $projectRoot -Parent) 'extensions\x4vr_hud'
if (Test-Path -LiteralPath (Join-Path $hud 'x4vr_hud.txt')) {
    Remove-Item -LiteralPath $hud -Recurse -Force
    Write-Output 'Removed the HUD distance extension.'
}
Write-Output 'In X4 you may turn "OpenTrack Support" off again (Options > Controls > Head Tracking Support).'
