# Notes for the next release

Entries for the next GitHub release notes, in release-note wording. Move them into the release
body when publishing, then clear this file.

<!-- Before release: headset check on 9.00 and 8.00. debug-events.log shows "TrackIR and Tobii
trackers off, X4 <version> code", head tracking works in the cockpit and on foot, and Ctrl+F11
toggles the theater screen. -->

### Head tracking frozen by other trackers

X4 supports several head trackers. Every frame it takes the pose from the last one in its list that reports data: OpenTrack, FreeTrack, TrackIR, then Tobii. The mod sends the headset pose through FreeTrack, so a TrackIR driver or a Tobii eye tracker on the same PC could take over. In the cockpit the picture then stayed locked to your head while the game ran normally. One player had vorpX's TrackIR emulation (`NPClient64.dll`), which X4 loads through a registry entry even when vorpX isn't running. On a Pimax Dream Air under SteamVR, the headset's Tobii eye tracker most likely caused the same freeze.

The mod now turns off X4's TrackIR and Tobii head tracking, on 9.00 and 8.00. You no longer need to rename registry entries or close other tracking software. This applies whenever X4 loads the mod's head tracker, also when you play without VR. To use TrackIR or Tobii head tracking in X4 again, run `X4_VR\scripts\uninstall.ps1` (see the README's *Uninstall* section).

The same fix brings back Ctrl+F11 for these players. Since v0.3.0, when another tracker won, the mod got no head pose from X4, kept the theater screen on and Ctrl+F11 couldn't turn it off.

Thanks to @Laffer for testing through many rounds of bug reports (#2).

### Higher frame rate in CPU-heavy saves

The mod holds X4 to the headset's refresh rate. Until now, a frame that took even slightly longer than one headset frame (11.1 ms at 90 Hz) waited for the next one, so X4 dropped straight from 90 to 45 fps. Late-game saves with big fleets, where the CPU is the limit, were hit hardest. A late frame now goes to the headset right away, so X4 runs at the frame rate your PC can reach. In a test with extra CPU load per frame, X4 went from 45 to 67 fps on both OpenVR and OpenXR, with no missed headset frames.

### Native Linux support (experimental)

X4 VR now runs on the native Linux version of X4 9.00, through SteamVR. Stereo works in the cockpit and on foot, along with head tracking, the flat screen for menus, the mouse cursor and the HUD distance setting. OpenXR isn't supported on Linux yet. The Flatpak version of Steam can't run the mod's launch script, so use your distribution's Steam package.

There's no download for Linux: you build the mod from the repository with CMake or Nix, as the README's *Linux* section describes. The `x4vr` terminal menu then checks your setup, keeps your VR settings and profiles, starts X4 in VR, writes bug reports and uninstalls the mod. X4 needs `x4vr-run %command%` as its Steam launch option. Steam's Play button still starts the normal 2D game; only *Launch X4 in VR* in the menu starts VR.

The port has been tested on one setup so far, a Steam Frame with an AMD GPU. If you play on NVIDIA or Intel, please attach a bug report from the menu to a GitHub issue, even if everything works.

Nothing changes for Windows players.

Thanks to @Cully-Curwen, who wrote and tested the port (#6).
