param(
    [ValidateSet('Smoke','Game')][string]$Target = 'Smoke',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateSet('build','build-next')][string]$BuildDirectory = 'build',
    [switch]$Memory,
    [switch]$NativeCamera,
    [switch]$StackTrace,
    [switch]$OpenVRBootstrap,
    [switch]$PoseHook,
    [switch]$HeadLook,
    [switch]$SceneHeadLook,
    [switch]$CrashWatch,
    [switch]$NoLayer,
    [string]$GameArgs = ''
)
$ErrorActionPreference = 'Stop'
if ($SceneHeadLook) { $HeadLook = $true }
if ($HeadLook) { $PoseHook = $true; $OpenVRBootstrap = $true }
if ($OpenVRBootstrap -and $NoLayer) { throw 'OpenVR bootstrap requires the Vulkan layer' }
if ($PoseHook -and $Target -ne 'Game') { throw 'The native pose hook requires the pinned X4 game executable' }
if ($PoseHook) { $CrashWatch = $true }
$projectRoot = Split-Path $PSScriptRoot -Parent
$binaryDirectory = Join-Path $projectRoot "$BuildDirectory/$Configuration"
if ($HeadLook -and -not (Test-Path -LiteralPath (Join-Path $binaryDirectory 'x4_openvr.dll'))) {
    throw 'Build the shared-runtime/head-look version first; older observation binaries cannot enable tracking'
}
$manifest = Join-Path $binaryDirectory 'VkLayer_x4vr_observe.json'
if (-not (Test-Path -LiteralPath $manifest)) { throw 'Build the observation layer first' }
$exe = Join-Path $binaryDirectory 'vulkan_smoke.exe'
$workingDirectory = $projectRoot
if ($Target -eq 'Game') {
    $workingDirectory = Split-Path $projectRoot -Parent
    $exe = Join-Path $workingDirectory 'X4.exe'
    if (Get-Process X4 -ErrorAction SilentlyContinue) { throw 'X4 is already running; this launcher does not attach to existing sessions' }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe" }
# Environment belongs only to this child process. No registry or persistent settings.
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exe
$start.WorkingDirectory = $workingDirectory
$start.UseShellExecute = $false
# Dependencies of the native module live beside it, not in the retail game root.
# This search-path addition is confined to this child and its crash recorder.
$start.Environment['PATH'] = $binaryDirectory + ';' + $env:PATH
if ($Target -eq 'Game') {
    # Preserve direct child launch (and its layer environment) under Steam's app context.
    $start.Environment['SteamAppId'] = '392160'
    $start.Environment['SteamGameId'] = '392160'
}
if (-not $NoLayer) {
    $start.Environment['VK_ADD_LAYER_PATH'] = $binaryDirectory + $(if ($env:VK_ADD_LAYER_PATH) { ';' + $env:VK_ADD_LAYER_PATH })
    $start.Environment['VK_INSTANCE_LAYERS'] = 'VK_LAYER_X4VR_observe' + $(if ($env:VK_INSTANCE_LAYERS) { ';' + $env:VK_INSTANCE_LAYERS })
} elseif ($Target -eq 'Smoke') { throw 'The smoke test explicitly requires the observation layer' }
$start.Environment['X4VR_CAPTURE_DIR'] = Join-Path $projectRoot 'reports/captures'
$start.Environment['X4VR_CAPTURE_MEMORY'] = $(if ($Memory -or $NativeCamera -or $StackTrace) { '1' } else { '0' })
$start.Environment['X4VR_CAPTURE_NATIVE_CAMERA'] = $(if ($NativeCamera) { '1' } else { '0' })
$start.Environment['X4VR_CAPTURE_STACK'] = $(if ($StackTrace) { '1' } else { '0' })
$start.Environment['X4VR_OPENVR_BOOTSTRAP'] = $(if ($OpenVRBootstrap) { '1' } else { '0' })
$start.Environment['X4VR_HEAD_LOOK'] = $(if ($HeadLook) { '1' } else { '0' })
$start.Environment['X4VR_GAME_ARGS'] = $GameArgs
$start.Environment['X4VR_SCENE_COPY'] = $(if ($SceneHeadLook) { '1' } else { '0' })
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
New-Item -ItemType Directory -Path $start.Environment['X4VR_CAPTURE_DIR'] -Force | Out-Null
if ($CrashWatch) {
    $start.FileName = Join-Path $binaryDirectory 'crash_watch_dev.exe'  # watchpoints, traces, -PoseHook
    if (-not (Test-Path -LiteralPath $start.FileName)) { throw 'Build the crash recorder first' }
    $debugDirectory = Join-Path $start.Environment['X4VR_CAPTURE_DIR'] ('debug-' + [Guid]::NewGuid().ToString('N'))
    # Windows PowerShell 5.1 (.NET Framework) lacks ProcessStartInfo.ArgumentList.
    $arguments = [Collections.Generic.List[string]]::new()
    $arguments.Add($exe)
    $arguments.Add($debugDirectory)
    if ($PoseHook) {
        $poseModule = Join-Path $binaryDirectory 'x4vr_native_pose.dll'
        if (-not (Test-Path -LiteralPath $poseModule)) { throw 'Build the native pose module first' }
        $arguments.Add('--startup-module')
        $arguments.Add($poseModule)
    }
    $start.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    Write-Output "External crash recorder reports: $debugDirectory"
}
$child = [Diagnostics.Process]::Start($start)
Write-Output "Observation process ID: $($child.Id)"
Write-Output "Capture root: $($start.Environment['X4VR_CAPTURE_DIR'])"
$logStem = "launcher-$($child.Id)-$([DateTime]::UtcNow.Ticks)"
$stdoutPath = Join-Path $start.Environment['X4VR_CAPTURE_DIR'] "$logStem.stdout.log"
$stderrPath = Join-Path $start.Environment['X4VR_CAPTURE_DIR'] "$logStem.stderr.log"
$stdoutFile = [IO.File]::Open($stdoutPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$stderrFile = [IO.File]::Open($stderrPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $stdoutTask = $child.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
    $stderrTask = $child.StandardError.BaseStream.CopyToAsync($stderrFile)
    $child.WaitForExit()
    [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdoutTask, $stderrTask))
} finally {
    $stdoutFile.Dispose()
    $stderrFile.Dispose()
}
Write-Output "Console logs: $stdoutPath ; $stderrPath"
if ($Target -eq 'Smoke') { Get-Content -LiteralPath $stdoutPath; Get-Content -LiteralPath $stderrPath }
if ($child.ExitCode -ne 0) { throw "Observation process exited with code $($child.ExitCode)" }
