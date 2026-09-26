# X4 Rebirth

Native stereoscopic VR with 6DOF head tracking for X4: Foundations, on any SteamVR (OpenVR) headset.

This is an unofficial fan project. It is not affiliated with or endorsed by Egosoft.

## What you need

- Windows 10 or 11 (64-bit), Steam, and X4: Foundations 9.00.
- SteamVR plus your headset's own software, for example Varjo Base with SteamVR support
  enabled.
- A GPU that can hold your headset's refresh rate (for example 90 fps). Every displayed frame
  renders one eye.
- An NVIDIA GPU is recommended, because the setup below uses DSR for a high render resolution.
  AMD users can try Virtual Super Resolution (untested).
- Visual Studio 2022 with the *Desktop development with C++* workload (it includes CMake), and
  Git. Python 3 is optional and only runs extra self-tests.

It was developed and tested with X4 9.00 from Steam, a Varjo Aero (Varjo Base + SteamVR), an
NVIDIA RTX 3090 holding a steady 90 fps at 3840×2160, and Windows 11. Other SteamVR headsets
(Index, Vive, Quest through Link or Virtual Desktop in SteamVR mode, and so on) should work but
haven't been tested. The calibration is specific to X4 9.00: on other versions head tracking
still works, but leaning backwards may be blocked (see [Limitations](#limitations)).

## Setup

### 1. Download and build

Clone the repository into your X4 installation folder, the one that contains `X4.exe`:

```bat
cd "C:\Program Files (x86)\Steam\steamapps\common\X4 Foundations"
git clone https://github.com/ToffelsKater/X4_Rebirth.git
```

Then run the installer from PowerShell:

```bat
powershell -ExecutionPolicy Bypass -File X4_Rebirth\scripts\install.ps1
```

The installer downloads the pinned dependencies (OpenVR SDK, Vulkan headers, MinHook), builds
everything and runs the self-tests; add `-SkipTests` to skip the tests. It then points the
per-user registry value `HKCU\Software\FreeTrack\FreeTrackClient\Path` at
`X4_Rebirth\build\Release`, which is where X4 looks for a FreeTrack head tracker. If a
different FreeTrack or opentrack path was set, it is backed up, and the uninstaller restores
it. Finally it creates the settings file `reports\captures\stereo.txt`. When it's done, the
launcher `X4VRLauncher.exe` sits directly in the `X4_Rebirth` folder.

### 2. Raise the render resolution with NVIDIA DSR

The headset gets the image X4 renders. At monitor resolution (1920×1080) that looks blurry in
VR, so let the game render at a higher resolution:

1. Open NVIDIA Control Panel → 3D Settings → Manage 3D settings → Global Settings.
2. Under DSR - Factors, tick 4.00x (native resolution), which turns 1920×1080 into 3840×2160.
   You can also tick DL 2.25x. With a 1440p monitor, 2.25x (3840×2160) is the sensible choice;
   4x (5120×2880) is very heavy.
3. Leave DSR - Smoothness at its default. It only affects the monitor view; the headset gets
   the full-resolution image.
4. Apply.

### 3. Set X4's options

Set these once in X4's Options menu; X4 saves them.

Options → Controls → Head Tracking Support:

| Setting | Value | Note |
| --- | --- | --- |
| OpenTrack Support | On | Enables X4's FreeTrack support, which the mod uses. "Waiting for OpenTrack connection" is normal. |
| FreeTrack → Head Rotation Factor | 100 % | Required: the mod expects 1:1. |
| FreeTrack → Head Position Factor | 100 % | Required. |
| FreeTrack → Head Motion Smoothing | any | The mod turns smoothing off internally, which the slider cannot do. |

Options → Display Settings:

| Setting | Value | Note |
| --- | --- | --- |
| Display Mode | Fullscreen | DSR resolutions only exist in fullscreen. |
| Resolution | 3840×2160 | Appears after enabling DSR. You can change it while playing. |
| Anti-Aliasing | None | A non-temporal mode (FXAA/MSAA/SSAA) is fine. Avoid *Temporal*. |
| AMD FSR | Off | Temporal upscalers mix left- and right-eye frames, which shows as ghosting. |
| NVIDIA DLSS | Off | Same reason. |
| VSync | Off | SteamVR paces the frames. |
| Frame Rate Limit | 90 FPS (your headset's refresh rate) or higher | |
| FOV | maximum (120°) | Required. The eye mapping is calibrated for it (see `game_tan_y` below). |

Options → Graphics Settings:

| Setting | Value |
| --- | --- |
| Chromatic Aberration | Off (recommended in VR) |
| Distortion | Off (recommended in VR) |
| Everything else | Whatever still holds a steady 90 fps |

You don't have to check the display and graphics settings by hand. The launcher in the next
step reads X4's `config.xml` and lists anything that doesn't match. Its *Fix X4 settings*
button corrects them after backing up `config.xml`, and only works while X4 is closed. The
head-tracking factors aren't stored in that file, so set those in the game.

### 4. First launch

1. Start your headset software and SteamVR. Steam must be running too.
2. Start `X4VRLauncher.exe` in the `X4_Rebirth` folder. Don't start X4 from the Steam library;
   launched that way the game runs flat.
3. Check the launcher's Status box: SteamVR should be running, and the head-tracking DLL path
   and the build should both read "ok". The X4 settings box should say that all settings
   match.
4. Press **Play X4 in VR**.
5. Load your game. Sit comfortably, look straight ahead and press Ctrl+F12 to recenter. The view
   also recenters on its own when head tracking starts.

Quit X4 normally when you're done.

## Playing

The launcher stays open while you play. Changes you make in it apply within half a second, and
it shows the frame rate the headset is getting. It keeps profiles with your VR mode, world
scale, prediction, stutter protection and the X4 resolution you want: type a name and press
*Save*. It comes with two, *Default* and *Pair 90 Hz (experimental)*.

You can also start the game without the launcher:
`powershell -ExecutionPolicy Bypass -File X4_Rebirth\scripts\play.ps1`

Always recenter with Ctrl+F12. X4's own *Reset Head Tracking* key breaks the calibration. After
a SteamVR *Reset seated position*, press Ctrl+F12 again.

Keep X4 focused on the desktop. Clicking into another window can throttle the game.

## Fine-tuning (optional)

`X4_Rebirth\reports\captures\stereo.txt` is re-read every half second while you play. The
launcher writes `stereo`, `pair`, `ipd_scale`, `predict` and `async_submit` for you; the other
values are calibrated for X4 9.00.

| Key | Default | Meaning |
| --- | --- | --- |
| `ipd_scale` | 1 | Eye-separation multiplier. Larger makes the world feel smaller. |
| `pos_scale` | 3.6 | Converts head movement into X4's units (1:1 in metres). |
| `yaw_gain`, `pitch_gain`, `roll_gain` | 2.1177, 2.1177, 3.1416 | Undo X4's internal angle scaling. |
| `delay` | 2 | Frames between reading a pose and showing that frame. |
| `predict` | 0.035 | Pose prediction in seconds. |
| `game_tan_y` | 0.8675 | Tangent of half the game's vertical FOV; 0.8675 matches FOV = 120°. |
| `stereo` | 1 | 0 = mono (the same image in both eyes). |
| `recenter` | 0 | Changing this number also recenters. |
| `async_submit` | 1 | Frames go to SteamVR from a separate thread, so a game stutter repeats the last image instead of flashing. 0 = old behaviour. |
| `pair` | 0 | Experimental: render both eyes back to back for 90 Hz per eye. Needs the game at 180 fps. |

## Limitations

Each eye gets 45 Hz, because frames alternate between the eyes. SteamVR reprojection keeps
rotation smooth, but fast head movement shows some parallax judder on nearby objects. The
experimental `pair=1` mode gives 90 Hz per eye but needs about twice the GPU power, and if the
game can't hold 180 fps it shows dark flashes.

The HUD and menus are part of the rendered image, so they don't float on their own VR layer.

On wide-FOV headsets there is a small black band at the very bottom of the view, because X4's
maximum FOV is a bit smaller than the Varjo Aero's.

VR only works when X4 is started through the launcher or `play.ps1`. A normal Steam launch runs
the game flat.

Only X4 9.00 is fully supported. X4 normally blocks leaning backwards, and the mod removes that
restriction in memory at runtime. On other versions it checks the exact code bytes, skips the
patch, and backward head movement stays blocked.

## Troubleshooting

Logs are written to `X4_Rebirth\reports\captures\debug-*\debug-events.log`; search for
`X4VR`.

| Problem | Check |
| --- | --- |
| No head tracking, or nothing in the headset | OpenTrack Support is On and X4 was started with the launcher or `play.ps1`. If the launcher's Status box says the head-tracking DLL path is not set, press *Fix head-tracking path* and start X4 again. The log should show `X4VR freetrack: first headset pose delivered`. |
| Headset shows only SteamVR's grey room | SteamVR must be running before the launch. The log should show `X4VR presenter: first stereo pair submitted`. |
| Blurry | Display Mode Fullscreen at 3840×2160 (DSR enabled). |
| Low frame rate or judder | Use a smaller DSR factor (e.g. 2560×1440) or lower graphics settings; 90 fps is needed. |
| World too big or too small | Adjust World scale in the launcher (`ipd_scale` in `stereo.txt`). |
| View off-center | Look straight ahead and press Ctrl+F12. |
| Game froze at startup (rare) | Close it and launch again. Disabling overlay hooks (Overwolf, OBS game capture) can help. |

## Uninstall

```bat
powershell -ExecutionPolicy Bypass -File X4_Rebirth\scripts\uninstall.ps1
```

This restores or removes the FreeTrack registry value. Then delete the `X4_Rebirth` folder.
If you like, set OpenTrack Support back to Off and restore your display settings.

## License

MIT (see [LICENSE](LICENSE)). The dependencies fetched at build time keep their own licenses:
OpenVR SDK (BSD-3-Clause), Vulkan-Headers (Apache-2.0), MinHook (BSD-2-Clause).
X4: Foundations is © Egosoft; this project contains no game files.
