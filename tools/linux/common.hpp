// What every part of the x4vr command needs: where the mod's files are.
#pragma once

#include <filesystem>

namespace x4vr::linux_launcher {
// The VR settings (stereo.txt): in $X4VR_DIR if set, else $XDG_STATE_HOME/x4vr, else ~/.local/state/x4vr.
std::filesystem::path settings_file();
}
