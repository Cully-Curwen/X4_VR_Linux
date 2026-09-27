$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$sdkPath = Join-Path $projectRoot 'external/openvr'
$sdkCommit = '0924064316de3effbcd1acf1e309182a2deb1c05'
if (-not (Test-Path -LiteralPath $sdkPath)) {
    git clone --filter=blob:none --sparse https://github.com/ValveSoftware/openvr.git $sdkPath
    if ($LASTEXITCODE -ne 0) { throw 'OpenVR SDK clone failed' }
    git -C $sdkPath sparse-checkout set headers lib/win64 bin/win64
    if ($LASTEXITCODE -ne 0) { throw 'OpenVR sparse checkout failed' }
    git -C $sdkPath checkout --detach $sdkCommit
    if ($LASTEXITCODE -ne 0) { throw 'Pinned OpenVR revision checkout failed' }
}
$revision = git -C $sdkPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $sdkCommit) {
    throw "Expected OpenVR $sdkCommit; found $revision. Existing checkout has not been changed."
}
Write-Output "OpenVR SDK: $revision"
$vulkanPath = Join-Path $projectRoot 'external/Vulkan-Headers'
$vulkanCommit = '6802bb4733b63ed5efd3adb308a6c885ef180ea1'
if (-not (Test-Path -LiteralPath $vulkanPath)) {
    git clone --filter=blob:none --sparse https://github.com/KhronosGroup/Vulkan-Headers.git $vulkanPath
    if ($LASTEXITCODE -ne 0) { throw 'Vulkan headers clone failed' }
    git -C $vulkanPath sparse-checkout set include
    if ($LASTEXITCODE -ne 0) { throw 'Vulkan sparse checkout failed' }
    git -C $vulkanPath checkout --detach $vulkanCommit
    if ($LASTEXITCODE -ne 0) { throw 'Pinned Vulkan revision checkout failed' }
}
$revision = git -C $vulkanPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $vulkanCommit) {
    throw "Expected Vulkan headers $vulkanCommit; found $revision. Existing checkout unchanged."
}
Write-Output "Vulkan headers: $revision"
$minhookPath = Join-Path $projectRoot 'external/minhook'
$minhookCommit = 'c3fcafdc10146beb5919319d0683e44e3c30d537'
if (-not (Test-Path -LiteralPath $minhookPath)) {
    git clone --filter=blob:none https://github.com/TsudaKageyu/minhook.git $minhookPath
    if ($LASTEXITCODE -ne 0) { throw 'MinHook clone failed' }
    git -C $minhookPath checkout --detach $minhookCommit
    if ($LASTEXITCODE -ne 0) { throw 'Pinned MinHook revision checkout failed' }
}
$revision = git -C $minhookPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $minhookCommit) {
    throw "Expected MinHook $minhookCommit; found $revision. Existing checkout unchanged."
}
Write-Output "MinHook: $revision"
$openxrPath = Join-Path $projectRoot 'external/OpenXR-SDK'
$openxrCommit = 'f2448a8797c85814aa892efc1ab8707900fbcc78' # release-1.1.63
if (-not (Test-Path -LiteralPath $openxrPath)) {
    git clone --filter=blob:none https://github.com/KhronosGroup/OpenXR-SDK.git $openxrPath
    if ($LASTEXITCODE -ne 0) { throw 'OpenXR SDK clone failed' }
    git -C $openxrPath checkout --detach $openxrCommit
    if ($LASTEXITCODE -ne 0) { throw 'Pinned OpenXR revision checkout failed' }
}
$revision = git -C $openxrPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $openxrCommit) {
    throw "Expected OpenXR SDK $openxrCommit; found $revision. Existing checkout unchanged."
}
Write-Output "OpenXR SDK: $revision"
