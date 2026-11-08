// X4's settings (its config.xml) for VR: the checks, fix-settings, and the resolution X4 should
// run at in VR.

#pragma once

#include "../launcher/launcher_settings.hpp"
#include "settings_swap.hpp"
#include <optional>
#include <string>
#include <vector>

namespace x4vr::linux_launcher {

// A size X4 renders at in VR.
struct Resolution {
    int width;
    int height;
};

// The common 16:9 sizes: offered in the menu, and the automatic one is the smallest that covers
// what SteamVR renders.
inline constexpr Resolution vr_resolutions[] = {{1920, 1080}, {2560, 1440}, {2880, 1620}, {3200, 1800}, {3840, 2160}};

// The VR resolution saved at the last ask (x4_resolution.txt: by fix-settings from SteamVR, or by the
// mod while X4 ran); none before the first.
std::optional<Resolution> read_last_vr_resolution();

// X4's VR settings, as config.xml text: config.xml itself during a VR session (the settings-mode
// marker), else the VR copy (config.xml.x4vr-vr). None without config.xml or before the first VR launch.
std::optional<std::string> read_x4_vr_settings();

// Whether the checks ask SteamVR for the resolution it renders at (a background OpenVR client while
// SteamVR runs; the answer is saved in x4_resolution.txt), or use the size saved at the last ask.
// fix-settings asks; the menu and the report, which check every few seconds or while X4 is stuck, don't.
enum class SteamVrQuery { ask, skip };

// The Windows launcher's checks (tools/launcher/launcher_settings.hpp, tested in launcher_tests),
// minus "fullscreen, not borderless": on Windows that is for NVIDIA DSR; on Linux X4's borderless
// window at the desktop resolution is fine.
std::vector<x4vr::launcher::Check> x4_checks(const std::string& xml, SteamVrQuery query);

// Fixes X4's VR settings for VR; `quiet` (x4vr-run): prints only what it changed and errors, no table.
int fix_x4_settings(bool quiet);

// Puts X4's VR settings (`vr`) or the standard settings in place (settings_swap.hpp); with the VR ones,
// the HUD extension is switched on. Waits up to 10 s for X4 to finish exiting. Nothing to do when
// X4 never started.
x4vr::linux_port::Switched switch_x4_settings(bool vr);

} // namespace x4vr::linux_launcher
