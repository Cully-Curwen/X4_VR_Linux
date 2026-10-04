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

---

## 0.4 How Steam runs native X4 on NixOS (2026-10-04)

### Facts

- Compatibility setting: default, **Steam Linux Runtime 3.0 (sniper)**.
- Launch chain: NixOS Steam FHS `bwrap` → `steam` → `reaper SteamLaunch AppId=392160` →
  `srt-bwrap` → `pv-adverb` → `testandlaunch` (an Egosoft **bash script** in the game folder) →
  the game, started as `./X4` from the game folder. The game names its main thread `Main()`, so
  `pgrep -x X4` finds nothing; use `pgrep -x 'Main\(\)'`.
- `PRESSURE_VESSEL_RUNTIME=sniper_platform_3.0.20260805.254768`, `PRESSURE_VESSEL_COPY_RUNTIME=1`.
- Loaded from the **host's Nix store** inside the container: `libc.so.6` from
  `glibc-2.42-84`, `libvulkan.so.1.4.357` (vulkan-loader 1.4.357), and Mesa 26.2.3's
  `libvulkan_radeon.so`. pressure-vessel took the host's newer glibc and graphics stack.
- `/nix/store` is visible inside the container, and so is `~/.config/openvr/openvrpaths.vrpath`.
- Environment set by pressure-vessel: `VK_LAYER_PATH` and `VK_IMPLICIT_LAYER_PATH` pointing at
  `/usr/lib/pressure-vessel/overrides/share/vulkan/…`, `VK_ICD_FILENAMES`/`VK_DRIVER_FILES`, and
  `LD_LIBRARY_PATH=lib:/usr/lib/pressure-vessel/overrides/…` (the game's own `lib/` first; the
  game ships its own libraries there, SDL3 among them).
- `LD_PRELOAD` already holds Steam's overlay (`/tmp/pressure-vessel-libs-…/${PLATFORM}/gameoverlayrenderer.so`).
  pressure-vessel rewrites preload entries into paths it makes visible.
- Session: Hyprland (Wayland), with Xwayland on `:0`. The game has both `WAYLAND_DISPLAY` and
  `DISPLAY` set; which one SDL3 uses is checked in 0.7.
- GPU: X4 opened `/dev/dri/renderD128`, which is PCI `0000:03:00.0`. The other GPU is
  `0000:13:00.0` (`renderD129`).

### Consequences

1. **The glibc risk is gone for this setup.** The game process uses NixOS's own glibc 2.42, so
   libraries built from the system's nixpkgs load without symbol-version problems. Static
   `libstdc++` is still worth doing, because the game's `lib/` comes first in `LD_LIBRARY_PATH`.
   This holds while pressure-vessel keeps choosing the host glibc (it picks the newer one), so
   `x4vr-run` should log the glibc version it sees.
2. **`/nix/store` paths work inside the container.** No copying to the home directory is needed.
   `x4vr-run` can point `LD_PRELOAD` and the layer path straight at the Nix package.
3. **Vulkan layer loading:** the loader is 1.4.357, so `VK_ADD_LAYER_PATH` is supported. It adds
   to pressure-vessel's own `VK_LAYER_PATH` instead of replacing it. Whether pressure-vessel
   passes `VK_ADD_LAYER_PATH` and `VK_INSTANCE_LAYERS` through unchanged is tested in 0.7.
4. **`LD_PRELOAD`:** `x4vr-run` runs outside the container (as `x4vr-run %command%`, where
   `%command%` includes the runtime's entry point), and prepends to the existing value. The
   preload library will be loaded into `testandlaunch`'s bash, `pv-adverb` and others too. It must
   stay inactive unless `/proc/self/exe` is the X4 binary (plan section 6.1).
5. **SDL3 comes from the game's own `lib/`.** A preloaded `SDL_PollEvent` still takes precedence.
6. The process name is `Main()`, not `X4`. Tools and the wrapper must look for it by
   executable path, not by name.

---

## 0.4 (continued) GPU, launch script, in-game options (2026-10-04)

### Facts

- `0000:03:00.0` is the Navi 31 (RX 7900 XT); `0000:13:00.0` is the Raphael integrated GPU. X4
  opens `renderD128` = the 7900 XT. In-game: *Auto-select GPU* on, graphics card
  "AMD Radeon RX 7900 XT (RADV NAVI31)".
- `testandlaunch` (Egosoft, bash): prepends `lib` to `LD_LIBRARY_PATH`, sets `GTK2_RC_FILES`,
  sources `testcommon` and checks for missing libraries and CA certificates (dialogs on failure).
  If an argument is `-prefer-wayland`, it sets `SDL_VIDEO_DRIVER=wayland,x11`; otherwise SDL3
  chooses. On a Wayland session it unsets `SDL_GAMECONTROLLER_IGNORE_DEVICES` (Steam Input).
  Finally `./X4 "$@"`: every argument reaches the game unchanged.
- Controls → *Head Tracking Support* offers only **OpenTrack Support** (off). FreeTrack, TrackIR
  and Tobii are not offered on Linux. The tracker filter, deadzone and factor options were not
  visible while OpenTrack is off; check them once it's on (0.5).
- Game settings → Camera: *Head Movement Intensity* 100, *VE Goggles Auto Reset* on. Both may act
  on head-tracking input; checked in 0.5.
- Display settings now: borderless window at the desktop's 3840x2160 (27" DP-1), TAA, FSR off,
  VSync off, frame-rate limit 120, **FOV 90°**.

### Consequences

1. OpenTrack UDP is the only tracker input on Linux, as planned. No rival-trackers patch needed.
2. Game arguments (`-skipintro -nocputhrottle`) can be passed through Steam's `%command%`;
   `testandlaunch` forwards them.
3. Whether the game uses Wayland or Xwayland by default is still to be logged in 0.7. The
   `-prefer-wayland` argument forces Wayland if needed.
4. Settings to change before VR tests, as on Windows: FOV to the maximum (120°, the
   `game_tan_y` 0.8675 calibration), anti-aliasing off or non-temporal (TAA history crosses
   eyes with alternate-eye rendering), FSR off. The desktop is already 4K, the resolution
   Windows reaches with DSR, so gamescope supersampling is optional at first.
5. Check whether *Head Movement Intensity* scales tracker input (keep it at 100 for calibration),
   and whether *VE Goggles Auto Reset* recenters on its own (it may need to be off, as X4's own
   reset breaks the calibration on Windows).
