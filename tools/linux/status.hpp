// What the menu's status page shows and the bug report records: Steam, SteamVR and X4 running,
// X4's launch option, the HUD extension, X4's VR settings, the GPU of the last VR session.

#pragma once

#include "code_scan.hpp"
#include "launch_option.hpp"
#include "x4_settings_rules.hpp"
#include <string>
#include <vector>

namespace x4vr::linux_launcher {

struct Status {
    bool steam = false, steamvr = false, x4 = false;
    // config.xml holds X4's VR settings now (the settings-mode marker exists): X4 runs in VR, or a VR
    // run ended without putting the 2D settings back.
    bool vr_settings_in_place = false;
    x4vr::linux_port::LaunchOption option;
    double hud = 0;            // installed HUD factor, 0: none
    bool hud_off_in_x4 = false; // installed but switched off in X4's Extensions menu
    int settings_to_fix = -1;  // -1: X4's config.xml not found, -2: VR settings made at the first VR launch
    struct Fix { x4vr::linux_port::Kind kind; std::string label, current; };
    std::vector<Fix> to_fix;   // required ones x4vr sets, optional ones and warnings for the player
};

// Reads all of it now: Steam's config, /proc, X4's config.xml and content.xml, the HUD extension.
Status read_status();

// The GPU of the last VR session, from the mod's logs (gpu_status.hpp). `state` as the menu's status
// rows (ui/terminal_ui.hpp): 0 ok, 1 warning, 2 problem, 3 neutral.
std::string read_gpu_summary(int& state);

// The X4 scan's result in words (code_scan.hpp: which of the mod's code sites this X4 has); `state`
// as above.
std::string make_scan_summary(const x4vr::linux_port::code::X4Sites& sites, int& state);

} // namespace x4vr::linux_launcher
