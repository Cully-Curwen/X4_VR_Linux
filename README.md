# X4 Native VR

Native stereoscopic, head-tracked (6DOF) VR for **X4: Foundations**, on any SteamVR (OpenVR)
headset.

- **Head tracking drives the game's own camera.** Culling, lighting, shadows and cockpit
  parallax are all correct.
- **Real 3D.** Each frame is rendered from the correct eye position.
- **Images go straight to SteamVR** with the pose they were rendered with. SteamVR's
  reprojection keeps the world stable when you turn your head.
- **No game files are modified.** Nothing is installed into the X4 folder beyond this repository.

> Unofficial fan project. Not affiliated with or endorsed by Egosoft.

## Tested setup

| | |
| --- | --- |
| Game | X4: Foundations **9.00** (Steam) |
| Headset | Varjo Aero (Varjo Base + SteamVR) |
| GPU | NVIDIA RTX 3090, steady 90 fps at 3840×2160 |
| OS | Windows 11 |

Other SteamVR headsets (Index, Vive, Quest via Link/Virtual Desktop in SteamVR mode, and so on)
should work, but they are untested. The calibration is specific to X4 9.00. On other versions
head tracking still works, but leaning backwards may be blocked (see *Limitations*).

## Requirements

- Windows 10/11 64-bit, X4: Foundations 9.00, Steam.
- SteamVR, plus your headset's own software (e.g. Varjo Base with SteamVR support enabled).
- A strong GPU. Every displayed frame renders one eye, so the game must hold your headset's
  refresh rate (e.g. 90 fps).
- NVIDIA GPU recommended, for DSR (high render resolution). AMD users can try Virtual Super
  Resolution (untested).
- To build: **Visual Studio 2022** with the *Desktop development with C++* workload (includes
  CMake), and **Git**. Python 3 is optional (extra self-tests).

## Installation

1. Clone this repository **into your X4 installation folder** (the folder that contains `X4.exe`):

   ```bat
   cd "C:\Program Files (x86)\Steam\steamapps\common\X4 Foundations"
   git clone https://github.com/ToffelsKater/X4_Native_VR.git
   ```

2. Run the installer from PowerShell:

   ```bat
   powershell -ExecutionPolicy Bypass -File X4_Native_VR\scripts\install.ps1
   ```

   It does four things:
   - Downloads pinned dependencies: OpenVR SDK, Vulkan headers, MinHook.
   - Builds everything and runs self-tests. Add `-SkipTests` to skip the tests.
   - Sets the per-user registry value
     `HKCU\Software\FreeTrack\FreeTrackClient\Path` to `X4_Native_VR\build\Release`. This is
     where X4 looks for a FreeTrack head tracker. If a different FreeTrack/opentrack path was
     set, it is backed up and restored by the uninstaller.
   - Creates the settings file `reports\captures\stereo.txt`.

## NVIDIA settings (resolution)

The headset receives the image X4 renders. At monitor resolution (1920×1080) it looks blurry
in VR, so let the game render at a higher resolution with **DSR**:

1. Open **NVIDIA Control Panel → 3D Settings → Manage 3D settings → Global Settings**.
2. **DSR - Factors**: tick **4.00x (native resolution)** (1920×1080 → 3840×2160). Optionally also
   tick **DL 2.25x**. With a 1440p monitor, 2.25x (3840×2160) is the sensible choice. 4x
   (5120×2880) is very heavy.
3. **DSR - Smoothness**: leave the default. It only affects the monitor view; the headset gets
   the full-resolution image.
4. Apply.

## In-game settings

Set these once in X4's **Options** menu. They are saved.

**Options → Controls → Head Tracking Support**

| Setting | Value | Note |
| --- | --- | --- |
| OpenTrack Support | **On** | This enables X4's FreeTrack support, which the mod uses. "Waiting for OpenTrack connection" is normal. |
| FreeTrack → Head Rotation Factor | **100 %** | Required: the mod expects 1:1. |
| FreeTrack → Head Position Factor | **100 %** | Required. |
| FreeTrack → Head Motion Smoothing | any | The mod turns smoothing off internally; the slider cannot. |

**Options → Display Settings**

| Setting | Value | Note |
| --- | --- | --- |
| Display Mode | **Fullscreen** | DSR resolutions only exist in fullscreen. |
| Resolution | **3840×2160** | Appears after enabling DSR. It can be changed while playing. |
| Anti-Aliasing | **None** | A non-temporal mode (FXAA/MSAA/SSAA) is fine. Avoid *Temporal*. |
| AMD FSR | **Off** | Temporal upscalers mix left- and right-eye frames (ghosting). |
| NVIDIA DLSS | **Off** | Same reason. |
| VSync | **Off** | SteamVR paces the frames. |
| Frame Rate Limit | **90 FPS** (your headset's refresh rate) or higher | |
| FOV | **maximum (120°)** | Required. The eye mapping is calibrated for it (see `game_tan_y` below). |

**Options → Graphics Settings**

| Setting | Value |
| --- | --- |
| Chromatic Aberration | Off (recommended in VR) |
| Distortion | Off (recommended in VR) |
| Everything else | Whatever still holds a steady 90 fps |

## Playing

1. Start your headset software and **SteamVR**. Steam must be running too.
2. Launch X4 through the VR launcher. Do not start it from the Steam library:

   ```bat
   powershell -ExecutionPolicy Bypass -File X4_Native_VR\scripts\play.ps1
   ```

3. Load your game. Sit comfortably, look straight ahead, and press **Ctrl+F12** to recenter.
   It also recenters automatically when head tracking starts.
4. Quit X4 normally when done.

Tips:

- **Use Ctrl+F12 for recentering.** X4's own *Reset Head Tracking* key breaks the calibration.
  After a SteamVR *Reset seated position*, press Ctrl+F12 again.
- **Keep X4 focused on the desktop.** Clicking into another window can throttle the game.

## Fine-tuning (optional)

`X4_Native_VR\reports\captures\stereo.txt` is re-read every half second while playing. The
defaults are calibrated for X4 9.00:

| Key | Default | Meaning |
| --- | --- | --- |
| `ipd_scale` | 1 | Eye-separation multiplier. Larger means the world feels smaller. |
| `pos_scale` | 3.6 | Converts head movement into X4's units (1:1 in metres). |
| `yaw_gain`, `pitch_gain`, `roll_gain` | 2.1177, 2.1177, 3.1416 | Undo X4's internal angle scaling. |
| `delay` | 2 | Frames between reading a pose and showing that frame. |
| `predict` | 0.035 | Pose prediction in seconds. |
| `game_tan_y` | 0.8675 | Tangent of half the game's vertical FOV; 0.8675 matches FOV = 120°. |
| `stereo` | 1 | 0 = mono (same image to both eyes). |
| `recenter` | 0 | Changing this number also recenters. |

## Limitations

- **45 Hz per eye.** Frames alternate between the eyes. SteamVR reprojection keeps rotation
  smooth, but fast head movement shows some parallax judder on nearby objects.
- **HUD and menus are part of the rendered image**, not a separate VR layer.
- **Small black band at the very bottom** of the view on wide-FOV headsets, because X4's
  maximum FOV is a bit smaller than the Varjo Aero's.
- **VR needs the launcher** (`play.ps1`); a normal Steam launch runs the game flat.
- **Only X4 9.00 is fully supported.** One X4 restriction is removed in memory at runtime:
  normally it blocks leaning backwards. On other versions this patch is skipped (after checking
  the exact code bytes) and backward head movement stays blocked.

## Troubleshooting

Logs are written to `X4_Native_VR\reports\captures\debug-*\debug-events.log`; search for
`X4VR`.

| Problem | Check |
| --- | --- |
| No head tracking | OpenTrack Support is On, X4 was started with `play.ps1`, and the registry path points to `build\Release`. The log should show `X4VR freetrack: first headset pose delivered`. |
| Headset shows only SteamVR's grey room | SteamVR must be running before the launch. The log should show `X4VR presenter: first stereo pair submitted`. |
| Blurry | Display Mode Fullscreen at 3840×2160 (DSR enabled). |
| Low frame rate / judder | Use a smaller DSR factor (e.g. 2560×1440) or lower graphics settings; 90 fps is needed. |
| World too big or too small | Adjust `ipd_scale` in `stereo.txt`. |
| View off-center | Look straight ahead and press Ctrl+F12. |
| Game froze at startup (rare) | Close it and launch again. Disabling overlay hooks (Overwolf, OBS game capture) can help. |

## Uninstall

```bat
powershell -ExecutionPolicy Bypass -File X4_Native_VR\scripts\uninstall.ps1
```

This restores or removes the FreeTrack registry value. Then delete the `X4_Native_VR` folder
and, if you like, set OpenTrack Support back to Off and restore your display settings.

## How it works

1. X4 supports FreeTrack head trackers. The mod's `FreeTrackClient64.dll` feeds X4 the
   SteamVR head pose, plus the current eye's offset.
2. A Vulkan layer, enabled only for the launched process, copies each finished frame into
   that eye's texture.
3. The layer submits both eye textures to SteamVR, with the correct per-eye field-of-view
   bounds and the pose each frame was rendered with.

X4's tracker quirks (smoothing, angle and position scaling, a backward-lean clamp) were
measured against the live game and are compensated. See [STATUS.md](STATUS.md) for the
technical details and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the research and
diagnostic tooling.

## License

MIT (see [LICENSE](LICENSE)). The dependencies fetched at build time keep their own licenses:
OpenVR SDK (BSD-3-Clause), Vulkan-Headers (Apache-2.0), MinHook (BSD-2-Clause).
X4: Foundations is © Egosoft; this project contains no game files.
