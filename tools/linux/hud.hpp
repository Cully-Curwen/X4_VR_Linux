// The HUD distance extension: built from the player's game files, in place for VR, set aside for 2D.

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace x4vr::linux_launcher {

// The factor chosen in the menu (stereo.txt hud_factor, 0: X4's default), or -1 if never chosen.
double read_hud_factor();

// HUD Scaled: chosen in the menu (stereo.txt hud_factor), else whether the extension is installed.
bool is_hud_scaled();

// The extension, and where it waits outside VR (settings_swap.hpp, place_hud).
std::filesystem::path hud_extension(const std::filesystem::path& game);

std::filesystem::path get_parked_hud(const std::filesystem::path& game);

// Its x4vr_hud.txt (scale=, source=), wherever it is now.
std::map<std::string, std::string> find_installed_hud(const std::filesystem::path& game);

// X4's per-user content.xml may have the extension turned off (<extension id="x4vr_hud"
// enabled="false"/>, from X4's Extensions menu or an older x4vr): turned on for VR. Outside VR
// the extension is moved aside instead (hud --park), as X4 often has no such file.
void set_hud_extension_enabled(const std::filesystem::path& config, bool on);

// hud --park (x4vr-run, for 2D: before Steam's Play and after a VR session): out of X4's sight.
int park_hud();

// hud --refresh (x4vr-run, at each VR launch): the factor chosen in the menu applied, and the
// extension rebuilt after a game update.
int refresh_hud();

} // namespace x4vr::linux_launcher
