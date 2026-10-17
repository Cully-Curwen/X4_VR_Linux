// X4 itself: where it's installed, its config, whether it's running.

#pragma once

#include <filesystem>

namespace x4vr::linux_launcher {

// Linux X4 keeps one config per Steam account under ~/.config/EgoSoft/X4/<id>/ (also looked for
// under $XDG_CONFIG_HOME when that is set elsewhere); the newest config.xml, empty if none.
std::filesystem::path x4_config();

// X4's game folder: $X4VR_GAME_DIR if set, else the first Steam library that has X4; empty if none.
std::filesystem::path x4_dir();

// X4's process id, 0 when it isn't running.
int x4_pid();

bool x4_running();

} // namespace x4vr::linux_launcher
