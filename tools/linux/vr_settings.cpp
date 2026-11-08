// The mod's VR settings the menu shows (vr_settings.hpp).

#include "vr_settings.hpp"
#include "common.hpp"
#include "settings_control.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace x4vr::linux_launcher {

// stereo.txt's value for `key` as a number, else its default.
static double read_number(const std::filesystem::path& path, std::string_view key) {
    for (const VrSetting& setting : vr_setting_table)
        if (setting.key == key) return std::atof(x4vr::linux_port::read_setting(path, key, setting.default_value).c_str());
    return 0;
}

VrSettings read_vr_settings() {
    const std::filesystem::path path = get_settings_file();
    VrSettings settings;
    settings.stereo = read_number(path, "stereo") != 0;
    settings.shared_pose = read_number(path, "shared_pose") != 0;
    settings.ipd_scale = read_number(path, "ipd_scale");
    settings.predict = read_number(path, "predict");
    settings.async_submit = read_number(path, "async_submit") != 0;
    settings.cursor = read_number(path, "cursor") != 0;
    settings.theater = int(read_number(path, "theater"));
    settings.theater_distance = read_number(path, "theater_distance");
    settings.theater_width = read_number(path, "theater_width");
    settings.x4_width = int(read_number(path, "x4_width"));
    settings.x4_height = int(read_number(path, "x4_height"));
    return settings;
}

bool write_vr_setting(std::string_view key, double value) {
    for (const VrSetting& setting : vr_setting_table) {
        if (setting.key != key) continue;
        char text[32];
        std::snprintf(text, sizeof text, "%.*f", setting.decimals, value);
        return x4vr::linux_port::write_setting(get_settings_file(), key, text);
    }
    return false;
}

} // namespace x4vr::linux_launcher
