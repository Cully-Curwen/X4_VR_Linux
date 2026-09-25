# X4 OpenVR development

Target: retail X4 9.00 on Windows x64, Varjo Aero through Varjo Base and SteamVR.

**Working VR (checkpoint 2026-09-25).** Stereoscopic, 6DOF head-tracked VR runs in
retail X4 on the Aero. Head pose enters X4 through its built-in FreeTrack support
(our `FreeTrackClient64.dll`). Frames alternate eyes, and the Vulkan layer submits them
to OpenVR. See `STATUS.md` → "How to run" and the measured X4 quirks table.
No on-disk game files are modified. Two things persist outside the process: one
per-user registry value (FreeTrack path) and X4's own head-tracking settings.
At runtime the DLL patches one branch in memory (the backward head-position clamp).

The sections below describe the earlier observation and diagnostic tooling.

## Implemented

- OpenVR scene session, compositor lifecycle and shutdown.
- Required Vulkan instance/device extension queries and headset GPU selection.
- Predicted headset poses, independent eye transforms (including canting), asymmetric
  Vulkan projection, standard/reverse depth, and yaw/position recentering.
- Paired Vulkan eye-image submission, frame identity checks, metadata validation,
  quit handling and tracking-loss handling.
- Standalone hardware probe and offline camera/metadata tests.
- Read-only PE export/import inspection and selective local renderer-source extraction.
- Process-scoped Vulkan observation layer, tested inside retail X4, capturing shader
  modules, pipeline layouts, sampled descriptors and presentation results.
- SPIR-V reflection of actual camera/object buffers, including matrix offsets/order.

## Build and probe

From this directory in PowerShell:

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
./build/Release/openvr_probe.exe
./build/Release/openvr_probe.exe --scene
```

`--scene` initializes a SteamVR scene application and tries for up to five seconds
to obtain tracked eye transforms. Individual runtime calls can block beyond that
retry deadline. It does not render X4 or a test scene. Use with Varjo Base running,
OpenVR enabled, Aero connected, and working base-station tracking. The default
probe only asks OpenVR about runtime installation and HMD presence.

The build uses the locally installed Visual Studio C++ compiler and Windows SDK.
The SDK is pinned to Valve OpenVR revision
`0924064316de3effbcd1acf1e309182a2deb1c05`. Valve's license is in
`external/openvr/LICENSE`. That dependency is fetched separately, not copied into
the game's executable directory.

The observation layer uses Khronos Vulkan-Headers pinned to
`6802bb4733b63ed5efd3adb308a6c885ef180ea1`; see that checkout's license files.

The experimental `x4_pose_detour` library uses MinHook v1.3.4, pinned to
`c3fcafdc10146beb5919319d0683e44e3c30d537`. Its license is preserved in
`external/minhook/LICENSE.txt` and copied beside the test executable. The library
is **not linked into the Vulkan observation layer**. The separate
`x4vr_native_pose.dll` module can be loaded with the opt-in mode below.
Its owned assembly fixture tests the observed ten-byte pose-update prologue,
selected-camera filtering, unchanged pass-through, rejected/throwing transforms,
recursion, concurrent callers, removal and reinstall. This does not validate the
full X4 function ABI. Installation/removal require externally established caller
quiescence on the same managing thread, outside loader callbacks. The exact-code
guard does not replace executable identity checks in a future game adapter.

## Experimental native camera-call capture

```powershell
./scripts/observe.ps1 -Target Game -PoseHook -NoLayer
```

This is **not VR/head tracking**. It only captures bounded, rigid input poses for
the selected derived camera, leaving the original pointer and matrix unchanged.
It requires the exact pinned X4 executable hash and matching in-memory signatures.
It never attaches to an existing session. The launcher temporarily places a
one-byte entry breakpoint in its new child, restores it, and holds the primary
thread there while loading the native module and calling its startup export.
This initialization runs outside DllMain, before the game's PE entry point.
The module is pinned until process exit; no live unload/removal is attempted.
Launch normally without `-PoseHook` to disable it on the next run.

`-PoseHook` implies the external crash recorder. That recorder remains read-only
unless this explicit startup-module mode is requested. `-NoLayer` leaves our
Vulkan observation layer disabled; it does not disable unrelated installed layers.
The camera binary records and format description go into a fresh
`reports/captures/pose-hook-<pid>-<tick>/` directory. Up to 320 records are written;
each record is 96 bytes. No saves/configuration/mods are changed by this module.

The first X4 trial installed the hook successfully, but the user closed the game
before any selected-camera sample was recorded. A second trial now records real
selected-camera calls in X4, with responsive startup rendering and consistent
view/inverse pairs. In the user-confirmed new-game cockpit, controlled left/right
free-look produces approximately -65/+65 degrees of local yaw, and looking up
produces -35 degrees of local pitch. Both records respond; the derived record has
small additional orientation offsets. This verifies the path in that cockpit,
not across all camera modes or prolonged gameplay. It still does not implement
tracked pose changes or stereo rendering.

## Experimental headset orientation

The replacement experiment uses the scene-camera producer, not the failed
derived-global hook:

```powershell
./scripts/build.ps1 -BuildDirectory build-next
./scripts/observe.ps1 -Target Game -SceneHeadLook -BuildDirectory build-next
```

It replaces ONLY the call at `0x77a376`, using a private nearby relay; the general
copy routine remains untouched. Exact executable hash, loaded argument-setup
bytes, original call target and rebuild prologue are checked before installation.
The original copy runs first. Eligible destinations must be camera-aligned slots
in the current producer half of the engine's 2x100 view pool, below its produced
count, and labeled exactly `class U::Zone`. The name alone is not sufficient.
On explicit enable, tracked orientation is composed with each freshly copied
pose. X4's native rebuild runs on an aligned private camera copy before commit,
using existing projection parameters and preserving translation. Other labels,
UI cameras and the source stack record are unchanged. Native culling is rebuilt,
but downstream object WVP/history and cockpit UI alignment remain unverified.
The same process-specific enable/disable/recenter/shutdown controls below apply.
It starts **disabled** and requires a new game process; no live attach or hot unload.
This replacement has not yet demonstrated visible camera movement or stereo.

**Latest in-game result: no visible cockpit movement.** The hook changes a
derived camera copy, but the renderer uses other camera objects. The controls
below reproduce that failed experiment; they are not working head tracking.

Build separately with `./scripts/build.ps1 -BuildDirectory build-next` while the
older game session is running. After exiting it, launch the new version with:

```powershell
./scripts/observe.ps1 -Target Game -HeadLook -BuildDirectory build-next
```

This implies the native pose hook and OpenVR Vulkan startup bridge. The shared
`x4_openvr.dll` owns one OpenVR session used by both modules. The launcher adds
the selected build directory only to the child's DLL search PATH. Tracking starts
**disabled**; no hotkeys are installed and no existing session is modified.
Once in the verified cockpit with the headset level, use the new X4 process ID:

```powershell
./build-next/Release/pose_control.exe <X4-pid> enable
./build-next/Release/pose_control.exe <X4-pid> recenter
./build-next/Release/pose_control.exe <X4-pid> disable
./build-next/Release/pose_control.exe <X4-pid> shutdown
```

Commands apply at the next selected-camera callback; the debug log acknowledges
them. Enable recenters yaw on the first valid foreground pose. Recenter preserves
head pitch/roll relative to gravity, as the stereo backend does. Disable restores
the game's unmodified pose; shutdown also releases the native module's runtime
reference and cannot be re-enabled that run. The Vulkan bridge may retain another
reference until instance destruction. Quit/tracking-loss handling falls back to
the native pose. Alt-tabbing also bypasses pose changes. Disable before testing
other camera modes, whose ownership is not verified yet.

This stage attempts desktop camera orientation only. It intentionally preserves
translation until world scale is calibrated, and does not submit X4 images to the
headset. It queries a current seated head pose without compositor pacing; actual
stereo rendering must use the frame-synchronized `WaitGetPoses` path instead.
The native hook/module remains pinned until process exit. Graceful runtime release
uses the explicit shutdown control; abrupt process exit does no DLL-destructor
cleanup. The user reported no desktop camera response despite logged pose
application. Visible camera integration remains unresolved.

## Observe X4's renderer

```powershell
./scripts/observe.ps1 -Target Smoke
./scripts/observe.ps1 -Target Game
./scripts/observe.ps1 -Target Game -Memory
./scripts/observe.ps1 -Target Game -NativeCamera
./scripts/observe.ps1 -Target Game -NativeCamera -CrashWatch
python tools/reflect_capture.py reports/captures/<process-directory> --output reports/x4_shader_reflection.json
python tools/analyze_uniforms.py reports/captures/<process-directory> --exe ../X4.exe --output reports/x4_uniform_analysis.json
```

Close an existing X4 session before starting the game capture. Steam must be running.
The game launcher sets the Steam app context and layer environment for its child
process only. It does not install a global layer or change launch options in Steam.
Exit X4 normally to end a capture. Running the game normally omits this layer.

Captures contain locally observed shader binaries and sampled API metadata under
`reports/captures/`. Keep them private: they include game shader code. Shader capture
is bounded to 4096 modules, 4 MiB per module, 128 MiB total; descriptor samples cover
the first 2048 update/bind calls. Presentation is sampled, not logged every frame.
Console output is redirected to log files to contain the installed mods' verbose
messages. The layer is for diagnostics and adds overhead; it does not enable stereo.

The optional `-Memory` switch tracks buffer allocation/binding/mapping and descriptor
lifetimes. It copies at most 2048 bytes from already mapped memory at sampled set
1/3 binds, up to 128 samples per slot. It never maps, invalidates, or changes game
memory. Reads use `ReadProcessMemory` against this process and are bounded to the
live allocation, mapping, buffer and descriptor range. CPU snapshots do not prove
GPU visibility or cockpit-camera identity. Memory-only mode does not walk the stack.
`-StackTrace` independently enables return-address collection (and implies memory
sampling); offsets are executable-relative. `-NativeCamera` performs its own
register unwind but does not imply `-StackTrace`. Capture headers record all three
options so comparison runs are distinguishable. Keep these captures private too.

`analyze_uniforms.py` checks camera matrix relationships and resolves containing
unwind ranges. These ranges can be function fragments, not callable entry points.
`disassemble_x4.py` optionally inspects bounded RVA ranges using Capstone 5.0.7,
installed locally under `external/python`; it does not load or modify the game.

`-NativeCamera` also enables memory tracing and reconstructs native registers at the
observed camera-upload return address. It captures the referenced camera structure,
wrapper, and stack-local uniform block without modifying them. Inspection is gated
by the exact supported X4 SHA-256 plus a loaded-code signature; unknown builds are
rejected. On the supported executable, native-camera mode samples up to 16 distinct
nonzero view/projection pairs per 120 presents, bounded to 512 read attempts in that
interval and 4096 camera samples per process. This replaces the ordinary camera
sample limit in native mode; object sampling retains its original limit.
This currently verifies view/inverse/projection source correspondence,
not player-camera ownership. Never treat a recovered pointer as valid after its
observed call returns.

`-CrashWatch` runs an external debug-event recorder and writes a small crash dump
on an unhandled second-chance exception. First-chance exceptions are passed to the
game's handlers; it does not inject code or change game memory. Debugging can alter
timing. Reports stay local in `reports/captures/debug-*`; dumps can contain private
process data, so do not publish them. A lost recorder detaches rather than killing
the game. `-NoLayer -CrashWatch -Target Game` permits a comparison run without
adding this project's layer (other pre-existing layers/settings are preserved).
Up to 4 MiB of debugger-routed game messages are retained with the event log.

`openvr_vulkan_probe.exe --submit-gray` is a separate submission diagnostic that
creates distinct eye images on the headset GPU and submits 90 neutral-gray frame
pairs. It takes SteamVR scene focus. On this RTX 3090/Varjo Aero setup it has now
submitted 90 frame pairs at 3292x2820 per eye with both RGBA8 and BGRA8 using
automatic color space. The earlier RGBA8/forced-linear run returned compositor
error 105. `--format rgba8|bgra8|rgba8-srgb|bgra8-srgb|rgba16f` selects a test
format; only the first two have been run. Running it with no arguments only prints
usage. This verifies API submission, not visual correctness, stereo scene rendering
or X4 integration. Runtime calls can block beyond its nominal 15-second deadline.

`vulkan_smoke` exercises a real GPU compute dispatch through the layer. It does not
exercise OpenVR image submission or X4's scene rendering.

`x4_eye_targets` owns two independent color/depth image-view pairs on a borrowed
Vulkan 1.1 device. Color targets support attachment rendering, sampling and copy
operations; depth targets support depth attachment, sampling and copy operations.
Each image has a dedicated device-local allocation. Unsupported formats/extents
fail explicitly, and partial construction releases its resources. The owner must
release compositor references and finish GPU work before destroying these targets;
the library neither owns the game device nor inserts a device-wide idle wait.

`openvr_vulkan_probe.exe --targets-only` uses this library without starting OpenVR.
On the RTX 3090 it created both 3292x2820 eye framebuffers, cleared/reused them in
render passes, and read back distinct RGBA8 pixels (10/10/10/255 and 20/20/20/255)
and D32F depths (0 and 0.5). The lifetime tests inject failure at all 20 format,
image, allocation, bind and view steps. This establishes resource ownership and
render-pass clear/readback behavior, not geometry drawing or X4 render-pass
compatibility. `--submit-gray` now uses these attachment-capable targets too; that
updated submission path has not yet been rerun with the headset. The previously
recorded 90-pair submissions used the earlier copy/clear-only image path.
`python tests/test_observe_modes.py build/Release` explicitly runs this GPU fixture
in API-only, memory-only, memory/stack, and memory/native modes. It verifies the
mode metadata, exact captured bytes, and opt-in stack behavior. This is not part
of the hardware-independent test suite and does not launch X4 or OpenVR.

`-OpenVRBootstrap` opts into the in-process OpenVR startup connection. It starts a
scene session, merges the runtime-required extensions into X4's instance/device
creation requests, and rejects a device on a different GPU from the headset.
Existing app extensions, queues, features and pNext chains are preserved. It does
not silently fall back to flat mode if OpenVR initialization fails. All runtime
calls and shutdown are serialized on Vulkan's calling thread (a worker-based
extension query hung inside the loader). No rendering session can migrate threads;
only the initialization-only bridge has that private capability. Runtime-internal
Vulkan creation does not recursively start another session. GPU matching compares
device UUIDs because the runtime and layer see differently wrapped handles.
The session is shared across live
instances and released before the last associated instance is destroyed.
This is initialization only: **no eye images, tracking waits, queue access or
stereo rendering are connected yet**. It takes SteamVR scene focus and needs a
fresh game launch. Do not combine it with another OpenVR diagnostic in that game
process. Disabled by default, it passed a real GPU/OpenVR startup, compute and
normal teardown test. X4 created its runtime-compatible device and reached the
intro with this connection; gameplay is not yet verified. Future submission must additionally serialize runtime
and application queue access and shut down before any submitted resources die.

## Renderer contract

`x4vr/x4_camera.hpp` now converts one captured native camera plus a runtime eye
pose/projection into typed per-eye matrices. It handles the observed +Z-forward,
infinite-reverse-Z projection, runtime asymmetry/cant, explicit units-per-metre,
and the observed clip-space jitter. Products accumulate in double precision to
reduce cancellation at large world translations. It also builds the finite
visibility projection and normalized world-space planes using captured near/far
distances, including a conservative current-jitter margin. The sphere visibility
helper accepts objects visible to either eye, not only objects visible to both.
Unsupported camera forms fail
closed. This is CPU-side construction only: it is not installed into a native
camera, does not draw either eye, and does not update culling, object WVP or history.
Player-camera identity, physical scale, per-eye viewport/jitter and render-pass
integration must be established before these matrices drive gameplay rendering.

`python tools/peek_camera.py --pid <X4 PID> --output <new-private-directory>` reads
the two known camera globals externally with query/read rights only. It checks
the retail executable hash and a loaded signature; no code is injected and no
threads are suspended. Repeated equal reads reduce but cannot eliminate tearing.
These records do not identify the player camera without controlled correlation.
Add `--render-links` to capture the selected-object camera, current camera pointer,
engine-context copy and 32 retained per-view wrapper slots. These fixed pointer
paths come from the pinned renderer's disassembly, not a memory scan. Null,
inaccessible or changed links are reported without retaining their records.
Pointer rechecks and repeated reads cannot guarantee an atomic snapshot or
exclude freed/reused memory; retained slots may be stale, not active this frame.
This mode is read-only and works with a normally launched, unmodified X4 process.
Known view-object paths also include a bounded diagnostic name hint when readable;
this helps distinguish scene, lens-flare and UI cameras, but is not a safe-write filter.
`x4_camera_tests.exe <camera-record.bin> ...` additionally tests eye composition
against such captures; the normal CTest case uses synthetic fixtures.

The future adapter must initialize `x4vr::Session` **before Vulkan instance/device
creation** and merge the required extension names into the game's enabled lists.
It must select the physical device returned by `output_device`.

Each frame must perform these steps on the render thread:

1. Serialize access to the graphics queue and call `begin_frame` for predicted poses.
2. Convert the returned right-handed metre-space eye views to the verified X4 camera
   convention. The matrices use row-major storage with column-vector multiplication.
   Projection assumes a positive-height Vulkan viewport and depth in `[0,1]`.
3. Render the same simulation state from both eye cameras into distinct images at
   the frame's recommended extent. Update culling, lighting, temporal histories,
   motion vectors, and camera-dependent post-processing consistently for each eye.
4. Resolve MSAA if necessary. Eye images need `TRANSFER_SRC | SAMPLED` usage. Submit
   graphics work with correct synchronization and transition both images to
   `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` before `submit`.
5. Call `submit(frame.id, images)` under the same graphics queue serialization used
   by the engine. Present the companion window and optionally call `post_present`.
6. Keep the Vulkan resources valid while OpenVR uses them. Shut the session down
   before destroying the submitted resources or their device/instance.

An invalid headset pose does not authorize rendering/submission. A failed eye
submission consumes the frame; start a new frame rather than retrying one eye.
Metadata checks cannot prove image contents, resource lifetime, GPU barriers, or
queue ownership. Those are the renderer adapter's responsibility. Eye images must
be independently rendered geometry; repeating the desktop image is not completion.

## Current evidence and next work

See [STATUS.md](STATUS.md) for the measured baseline, missing integration, and
acceptance criteria. Tests do not establish that X4 supports VR.

Reference checkouts under `reference/` are research only and are not installed.
Extracted game sources under that directory remain local; do not distribute them.
