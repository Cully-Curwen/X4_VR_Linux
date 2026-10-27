// The menu's profiles: named sets of the VR settings it shows. Built in: share/x4vr/profiles/<name>.txt
// (installed); the player's own: <state>/profiles/<name>.txt. A profile holds stereo.txt lines;
// loading one writes them to stereo.txt. The profile last loaded or saved is named in <state>/profile.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace x4vr::linux_launcher {

struct Profile {
    std::string name;
    bool built_in;
    std::filesystem::path file;
};

// Where the player's own profiles are saved.
std::filesystem::path get_user_profiles_dir();

// Every profile, built-in ones first, each by name; a player's profile with a built-in one's name is
// left out.
std::vector<Profile> find_profiles();

// The profile last loaded or saved; empty if none.
std::string read_current_profile();

// Whether stereo.txt has what the profile sets (numbers compared as numbers).
bool is_profile_applied(const Profile& profile);

// Writes the profile's settings to stereo.txt and names it the current one; false if a write failed.
bool load_profile(const Profile& profile);

// Saves stereo.txt's settings as the player's profile `name`; what it did, in a line for the menu.
std::string save_profile(const std::string& name);

// Deletes the player's profile `name` (no longer the current one, if it was); stereo.txt stays as it is.
void delete_profile(const std::string& name);

} // namespace x4vr::linux_launcher
