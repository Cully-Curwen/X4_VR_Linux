# Linux port: Phase 0 findings

Measurements for `docs/LINUX_PORT_PLAN.md`. One section per Phase 0 step; each records the raw
facts first, then what they mean for the plan.

Machine: NixOS, AMD Ryzen 9 7900X3D (with integrated RDNA2 GPU), AMD Radeon RX 7900 XT,
Mesa 26.2.3 (RADV), Steam Frame through SteamVR. X4 9.00 native Linux build from Steam.

---

## 0.3 The game binary (2026-10-04)

### Facts

- `X4`: `ELF 64-bit LSB executable, x86-64`, **not PIE**, dynamically linked, **stripped**.
  BuildID `77cf7ac0ff32d88ffd3db7eb6ea2e025aecb711d`.
- SHA-256 `113f73d74570084f6ce18a659b365009eccf579fb66dbabb5dad2b1718045f20`, `version.dat` 900.
- Linked dynamically, among others: `libSDL3.so.0`, `libSDL3_ttf.so.0`, `libluajit-5.1.so.2`,
  `libvulkan.so.1`, `libsteam_api.so`, `libopenal.so.1`, `libpthread.so.0`, `libdl.so.2`,
  FFmpeg (`libavcodec.so.55` and others).
- 28,644 dynamic exports. Every export the Windows mod uses is present:
  `Get/SetActiveHeadTrackerHeadFilterStrength`, `IsFullscreenMenuDisplayed`,
  `IsPlayerControllingShip`, `IsHeadTrackingActive`, `IsFullscreenCutsceneActive`, plus
  `IsVRMode` and the other `ActiveHeadTracker` getters and setters.
- Tracker strings: `HEADTRACK_{NONE,DISABLE,FREETRACK,OPENTRACK,TOBII,TRACKIR,DUMMYVR,DUMMY_NOVR}`,
  `OpenTrackState` with `OPENTRACK_{DISABLED,PENDING,CONNECTED,ERROR,SHUTDOWN}`,
  `GetOpenTrackConnectionStatus`, `Get/SetOpenTrackSupportOption`, `IsOpenTrackEnabled`,
  `enableopentrack`, `OpenTrackThread`.
- C++ RTTI type names are still in the binary: `N2VR9OpenTrackE` (`VR::OpenTrack`),
  `N2VR17OpenTrackRunnableE` (`VR::OpenTrackRunnable`), `N1U23HeadTrackerCameraBridgeE`
  (`U::HeadTrackerCameraBridge`).

### Consequences

1. **Not PIE:** the executable always loads at its link address. Signature hits are absolute
   addresses; no load-base arithmetic is needed. Keep the signature checks anyway.
2. **RTTI gives the vtables directly.** In the Itanium C++ ABI a vtable's slot -1 points to the
   class's `type_info`, which points to the name string. So `VR::OpenTrack`'s vtable can be found
   from its name: string → `type_info` → vtable. The Windows hooks swap slots **in the vtable**
   (`swap_slot`), not per object, so the Linux hooks don't need the tracker object at all. The
   object arrives as `this` when X4 calls the hooked slot. This replaces the plan's "find the
   tracker from the recv buffer" step.
3. **`U::HeadTrackerCameraBridge`** is the class whose code holds the backward clamp, the
   still check and the on-foot zeroing on Windows. Its vtable, found the same way, leads to its
   methods, which makes stage D much easier.
4. **OpenTrack is read on its own thread** (`OpenTrackThread`, `OpenTrackRunnable`). This is the
   "separate reader thread" case. With eye at use it doesn't affect which eye is shown, but it
   matters in two places:
   - the per-frame logic Windows runs in `FTGetData` (game thread) needs another game-thread
     hook. Candidate: the `VR::OpenTrack` vtable method the game thread calls to fetch the latest
     pose, the counterpart of the Windows code that calls `FTGetData`;
   - the read-time "pull" idea in the plan (section 6.2) gives no benefit. The shim only needs to
     send centre-pose packets.
5. **SDL3, not SDL2,** linked dynamically. Hotkeys: wrap SDL3's `SDL_PollEvent` (and
   `SDL_PeepEvents`) in the preload shim. Note that SDL3's event struct and key codes differ from
   SDL2's.
6. **All needed exports exist,** so smoothing off and the theater decision (stage E) need no
   reverse engineering.
7. FreeTrack, TrackIR and Tobii still appear as enum names. Whether the Linux build can use any of
   them is checked in step 5 (the options screen). The rival-trackers patch is probably unneeded.

---

## 0.2 Vulkan queue families (2026-10-04)

### Facts

`vulkaninfo` lists three devices: the **RX 7900 XT** (RADV NAVI31), the **CPU's integrated GPU**
(RADV RAPHAEL_MENDOCINO) and `llvmpipe`. On the 7900 XT:

| Family | Count | Flags | Present |
|---|---|---|---|
| 0 | **1** | graphics, compute, transfer, sparse | yes |
| 1 | 4 | compute, transfer, sparse | yes |
| 2 | 1 | video decode | no |
| 3 | 1 | video encode | no |
| 4 | 1 | sparse only | no |

(`libvulkan_dzn.so` fails to load with `-9`: that's Mesa's Direct3D 12 driver for WSL, harmless.)

### Consequences

1. **The private-queue problem is confirmed.** The layer only adds a queue to a graphics family
   with a free slot (`observe_layer.cpp:388`). Family 0 has one queue, so on this GPU:
   - with OpenVR, asynchronous submission silently turns off;
   - with OpenXR, the layer stops with "OpenXR needs a private queue".
2. **Plan option 1 fits:** family 1 has four compute queues. The layer's work there is image
   copies and submission, which need no graphics. Needed: queue-family ownership transfers (or
   `VK_SHARING_MODE_CONCURRENT` eye textures) between family 0 and family 1, and a check that
   SteamVR accepts a compute-family queue in `VRVulkanTextureData_t`. Selection rule: use the
   game's graphics family if it has a free queue (unchanged Windows path); otherwise a
   compute family.
3. **Two RADV GPUs.** X4 and SteamVR must use the 7900 XT. The layer already compares device
   UUIDs with the runtime's output device. Step 4 checks which GPU X4 picks.
   `MESA_VK_DEVICE_SELECT` can force it from the wrapper if needed.

---

## 0.1 SteamVR and the Steam Frame (2026-10-04, partial)

### Facts

- SteamVR runs with the Steam Frame on this machine, but **does not start on its own** when an
  application asks for it (a known Linux issue). It has to be started by hand first. SteamVR's
  desktop sharing doesn't work either.
- `~/.config/openvr/openvrpaths.vrpath` registers the runtime at
  `~/.local/share/Steam/steamapps/common/SteamVR`, with config and logs under
  `~/.local/share/Steam/{config,logs}`. No external drivers.
- `~/.config/openxr/1/active_runtime.json` points to SteamVR
  (`SteamVR/bin/linux64/vrclient.so`), so `X4VR_RUNTIME=openxr` will also reach SteamVR.

### Consequences

1. Launch order is always SteamVR first, then X4. `x4vr-run` checks that SteamVR is running
   (`vrserver` process) before starting X4 and says so clearly if it isn't. Without SteamVR
   the mod can't find a headset, and X4 runs flat as without the mod.
2. Desktop sharing isn't used by the mod. The theater screen and cursor are the mod's own
   OpenVR overlays (or OpenXR quad layers). Their behaviour on Linux SteamVR is tested in 0.1
   with `runtime_smoke`.
3. Both runtime files live under the home directory, so a Steam container normally sees them.
   Step 0.4 confirms this.
