// What every part of the x4vr command needs: where the mod's files are, reading and writing files.

#pragma once

#include <filesystem>
#include <string>

namespace x4vr::linux_launcher {

// The VR settings (stereo.txt): in $X4VR_DIR if set, else $XDG_STATE_HOME/x4vr, else ~/.local/state/x4vr.
std::filesystem::path settings_file();

// The mod's state folder: logs, the launch request, the 2D/VR marker. For now the same folder as
// stereo.txt; it gets its own lookup when stereo.txt moves to the config folder.
std::filesystem::path state_dir();

// X4's settings for 2D and for VR, kept apart (settings_swap.hpp); the marker names the config
// while X4 has its VR settings.
std::filesystem::path settings_marker();

// A whole file as text; empty if it can't be read.
std::string read_text(const std::filesystem::path& path);

// Replaces a file atomically (a temporary, then a rename): a crash mid-write leaves the old one,
// never half of X4's content.xml.
bool write_text(const std::filesystem::path& path, const std::string& text);

} // namespace x4vr::linux_launcher
