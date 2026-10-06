# X4 VR on Linux

Stereo VR for the native Linux X4: Foundations (9.00) through SteamVR. Experimental; not
affiliated with Egosoft.

## Requirements

- X4 (native Linux version from Steam), started once.
- SteamVR with your headset working. Not the Flatpak Steam.
- To build: GCC 13+ or Clang 16+, CMake 3.24+, git, Vulkan headers.

## Install

```bash
git clone https://github.com/Cully-Curwen/X4_VR_Linux ~/x4vr-src
git clone --depth 1 https://github.com/ValveSoftware/openvr ~/x4vr-src/external/openvr
cd ~/x4vr-src
cmake -S . -B build -DX4VR_LINUX=ON -DCMAKE_BUILD_TYPE=Release \
      -DOPENVR_SOURCE_DIR=$PWD/external/openvr -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j && cmake --install build
```

Nix: `nix-build` in the source directory.

## Set up

Run `~/.local/bin/x4vr`. The menu explains each item at the bottom of the screen; `x4vr help`
lists the same actions as commands.

1. **Copy the Steam launch option** and paste it into X4 > Properties > General > Launch options.
   Steam's Play button still starts the normal 2D game.
2. Optional: **Add to the app launcher**.
3. On a tiling window manager (Hyprland, Sway, i3): **Tiling window manager rules** has the
   rule to add.

## Play

1. **Launch X4 in VR** in the menu. It starts SteamVR if needed.
2. Look ahead and press **Ctrl+F12** to recentre. **Ctrl+F11** toggles the flat screen.

Your 2D X4 settings are kept separately and restored when X4 closes.

## Problems

- Log: `~/.local/state/x4vr/x4vr.log`.
- **Make a bug report** in the menu, and attach the file to a
  [GitHub issue](https://github.com/Cully-Curwen/X4_VR_Linux/issues).

## Uninstall

1. **Uninstall** in the menu.
2. Clear X4's launch option in Steam.
3. Delete the clone directory.
