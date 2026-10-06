# Features: Windows and Linux

Linux status: **Works** (tested with a Steam Frame, SteamVR, X4 9.00), **Missing**, **Not needed**
or **Planned**. A "No" under Windows means Linux only.

## VR and rendering

| Feature | Windows | Linux | Note |
|---|---|---|---|
| Alternate-eye stereo | Yes | Works | |
| Eye images submitted with their pose, async submission | Yes | Works | |
| Eye chosen at the tracker read (eye at use) | Yes | Works | |
| Shared pose per eye pair | No | Works | Fixes the Steam Frame's right-eye ghosting. |
| Eye images at SteamVR's recommended size | No | Works | |
| Waits for the headset at start | No | Works | Up to 2 minutes. |
| OpenXR | Yes | Missing | Only needed without SteamVR. |
| Turn compensation | Yes | Not needed | The shared pose avoids the double image. |

## Head tracking and X4 patches

| Feature | Windows | Linux | Note |
|---|---|---|---|
| Head tracking | FreeTrack DLL | Works | OpenTrack over UDP. |
| Smoothing off, backward clamp patch | Yes | Works | |
| X4 code found by byte pattern | Yes | Works | `x4vr patterns` checks a new X4 build. |
| TrackIR / Tobii patch | Yes | Not needed | Linux X4 only has OpenTrack. |
| Recenter (hotkey, SteamVR's recenter) | Hotkey | Works | |

## On foot

| Feature | Windows | Linux | Note |
|---|---|---|---|
| Stereo and head tracking on foot | Yes | Works | Same patches as Windows. |
| Snap turning | No | Not needed | |

## Menus, screen and input

| Feature | Windows | Linux | Note |
|---|---|---|---|
| Flat screen for menus, Ctrl+F11 | SteamVR overlay | Works | Drawn into the eye images (overlays don't show on the Frame). |
| Mouse cursor | Yes | Works | |
| SteamVR's *Exit game* closes X4 | No | Works | |
| Own window class in VR (`X4VR`) | No | Works | For tiling window manager rules. |

## HUD and X4 settings

| Feature | Windows | Linux | Note |
|---|---|---|---|
| HUD distance | Yes | Works | Linux also patches X4's precompiled `.xpl` UI scripts. |
| X4 settings checked and fixed | Yes | Works | Windowed mode on Linux. |
| 2D and VR settings kept apart | No | Works | Restored after a crash too. |
| In-game settings checklist | README | Works | |

## Launcher and tools

| Feature | Windows | Linux | Note |
|---|---|---|---|
| Launcher | Window | Works | Terminal menu, `x4vr`. |
| Profiles | Yes | Works | Built-in *Steam Frame*. |
| X4 resolution, custom size | Yes | Works | |
| Notices (known issues, tips) | No | Works | |
| Bug report | Yes | Works | |
| Install and uninstall | Yes | Works | |
| Crash recorder | Yes | Missing | Linux logs the exit status. |
| Diagnostics | Full set | Partial | Traces, eye dump, pair stats. |

## Planned

| Feature | Note |
|---|---|
| Automatic resolution in the headset's shape | About 10% fewer pixels on the Steam Frame (2640×1588 instead of 2880×1620). |
| HUD sized by X4's own UI scale | Would avoid editing X4's UI scripts, so no "modified" flag and Protected UI Mode can stay on. |
| Curved flat screen | Every part faces you, so the corners are as readable as the centre. |
| Both eyes from one game step | Holding X4's clock for the second frame of a pair. |
| Eye check from X4's camera matrix | A wrong-eye counter for new X4 versions. |
