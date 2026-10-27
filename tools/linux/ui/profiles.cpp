// The menu's profiles: named sets of the VR settings it shows (profiles.hpp).

#include "ui/profiles.hpp"
#include "common.hpp"
#include "install.hpp"
#include "settings_control.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace x4vr::linux_launcher {

// A setting a profile holds, and the mod's default for it (runtime_bootstrap.hpp) when stereo.txt has none.
struct ProfileSetting {
    const char* key;
    const char* fallback;
};

// The settings a profile holds: the ones the menu shows. hud_factor's empty default: X4's own HUD size.
static const ProfileSetting profile_settings[] = {
    {"stereo", "1"}, {"shared_pose", "1"}, {"ipd_scale", "1"}, {"predict", "0.035"}, {"async_submit", "1"}, {"cursor", "1"},
    {"theater", "1"}, {"theater_distance", "2"}, {"theater_width", "2.2"}, {"x4_width", "0"}, {"x4_height", "0"}, {"hud_factor", ""}};

// Names the profile last loaded or saved.
static bool write_current_profile(const std::string& name) { return write_text(get_state_dir()/"profile", name+"\n"); }

std::filesystem::path get_user_profiles_dir() { return get_state_dir()/"profiles"; }

std::vector<Profile> find_profiles() {
    std::vector<Profile> list;
    for (const bool built_in : {true, false}) {
        // This folder's profiles, by name.
        std::vector<Profile> found;
        std::error_code error;
        const std::filesystem::path dir = built_in ? get_data_dir()/"profiles" : get_user_profiles_dir();
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, error))
            if (entry.path().extension() == ".txt") found.push_back({entry.path().stem().string(), built_in, entry.path()});
        std::sort(found.begin(), found.end(), [](const Profile& a, const Profile& b) { return a.name < b.name; });

        // Added unless a profile of that name is listed already.
        for (const Profile& profile : found) {
            bool listed = false;
            for (const Profile& other : list)
                if (other.name == profile.name) listed = true;
            if (!listed) list.push_back(profile);
        }
    }
    return list;
}

std::string read_current_profile() {
    std::string name = read_text(get_state_dir()/"profile");
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    return name;
}

bool is_profile_applied(const Profile& profile) {
    for (const ProfileSetting& setting : profile_settings) {
        const std::string wanted = x4vr::linux_port::read_setting(profile.file, setting.key, "");
        if (wanted.empty()) continue;
        const std::string now = x4vr::linux_port::read_setting(get_settings_file(), setting.key, setting.fallback);
        if (now.empty() || std::fabs(std::atof(now.c_str())-std::atof(wanted.c_str())) > 1e-6) return false;
    }
    return true;
}

bool load_profile(const Profile& profile) {
    bool ok = true;
    for (const ProfileSetting& setting : profile_settings) {
        const std::string value = x4vr::linux_port::read_setting(profile.file, setting.key, "");
        if (!value.empty()) ok = x4vr::linux_port::write_setting(get_settings_file(), setting.key, value) && ok;
    }
    return write_current_profile(profile.name) && ok;
}

std::string save_profile(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name[0] == '.') return "Not a usable name: "+name;
    for (const Profile& profile : find_profiles())
        if (profile.built_in && profile.name == name) return "\""+name+"\" is built in: pick another name.";

    // stereo.txt's values for the profile's settings, as stereo.txt lines.
    std::string text = "# x4vr profile, saved from the menu\n";
    for (const ProfileSetting& setting : profile_settings) {
        const std::string value = x4vr::linux_port::read_setting(get_settings_file(), setting.key, setting.fallback);
        if (!value.empty()) text += std::string(setting.key)+"="+value+"\n";
    }

    std::error_code error;
    std::filesystem::create_directories(get_user_profiles_dir(), error);
    const std::filesystem::path file = get_user_profiles_dir()/(name+".txt");
    if (!write_text(file, text)) return "Can't write "+file.string();
    write_current_profile(name);
    return "Saved as profile \""+name+"\".";
}

void delete_profile(const std::string& name) {
    std::error_code error;
    std::filesystem::remove(get_user_profiles_dir()/(name+".txt"), error);
    if (read_current_profile() == name) std::filesystem::remove(get_state_dir()/"profile", error);
}

} // namespace x4vr::linux_launcher
