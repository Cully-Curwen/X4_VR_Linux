# Ready-to-play download: dist\X4_VR-<version>.zip. Players extract it into the X4 folder (next to
# X4.exe) and start X4_VR\X4VRLauncher.exe; no Visual Studio or Git needed. The version is the
# release tag on HEAD (v0.1.0), else tag-count-commit, with -modified for uncommitted changes.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -BuildDirectory build }

$version = (git -C $projectRoot describe --tags --always --dirty=-modified).Trim()
$dist = Join-Path $projectRoot 'dist'
$stage = Join-Path $dist 'X4_VR'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }

# Same layout as a source checkout, so the launcher and scripts work unchanged.
$files = @('X4VRLauncher.exe', 'README.md', 'LICENSE', 'config\stereo.txt', 'scripts\play.ps1', 'scripts\uninstall.ps1') +
    (Get-ChildItem (Join-Path $projectRoot 'config\profiles') -Filter *.txt | ForEach-Object { "config\profiles\$($_.Name)" }) +
    ('VkLayer_x4vr_observe.json', 'x4vr_observe.dll', 'x4_openvr.dll', 'FreeTrackClient64.dll', 'openvr_api.dll', 'crash_watch.exe' |
        ForEach-Object { "build\Release\$_" })
foreach ($file in $files) {
    $target = Join-Path $stage $file
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) $target
}
$licenses = @{ 'external\openvr\LICENSE' = 'OpenVR.txt'; 'external\minhook\LICENSE.txt' = 'MinHook.txt'; 'external\Vulkan-Headers\LICENSE.md' = 'Vulkan-Headers.md' }
New-Item -ItemType Directory -Path (Join-Path $stage 'licenses') -Force | Out-Null
foreach ($source in $licenses.Keys) { Copy-Item -LiteralPath (Join-Path $projectRoot $source) (Join-Path $stage "licenses\$($licenses[$source])") }
Set-Content -LiteralPath (Join-Path $stage 'version.txt') -Value $version -Encoding ascii

$zip = Join-Path $dist "X4_VR-$version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
tar.exe -a -c -f $zip -C $dist X4_VR  # forward-slash entries; Compress-Archive (PowerShell 5) writes backslashes
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
Write-Output "Package: $zip"
