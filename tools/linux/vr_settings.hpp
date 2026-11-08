// The mod's VR settings the menu shows and a profile holds, in stereo.txt (get_settings_file()):
// their keys, the mod's defaults (src/runtime_bootstrap.hpp) and their values.

#pragma once

#include <string_view>

namespace x4vr::linux_launcher {

// A setting the menu shows: its key, the mod's default when stereo.txt has none, and the decimals
// its value is written with.
struct VrSetting {
    const char* key;
    const char* default_value;
    int decimals;
};

// hud_factor's empty default: X4's own HUD size.
inline constexpr VrSetting vr_setting_table[] = {
    {"stereo", "1", 0}, {"shared_pose", "1", 0}, {"ipd_scale", "1", 2}, {"predict", "0.035", 3}, {"async_submit", "1", 0},
    {"cursor", "1", 0}, {"theater", "1", 0}, {"theater_distance", "2", 2}, {"theater_width", "2.2", 2}, {"x4_width", "0", 0},
    {"x4_height", "0", 0}, {"hud_factor", "", 1}};

// The settings' values (hud_factor aside: hud.hpp).
struct VrSettings {
    bool stereo;
    bool shared_pose;
    double ipd_scale;
    double predict;          // seconds
    bool async_submit;
    bool cursor;
    int theater;             // the flat screen: 0 never, 1 automatic, 2 always
    double theater_distance; // metres
    double theater_width;    // metres
    int x4_width;            // 0: automatic
    int x4_height;
};

// stereo.txt's values, each its default when the file has none.
VrSettings read_vr_settings();

// Writes `value` for `key` to stereo.txt, with the key's decimals (1 and 0 for on and off); false if
// it can't be written, or `key` isn't one of the table's.
bool write_vr_setting(std::string_view key, double value);

} // namespace x4vr::linux_launcher
