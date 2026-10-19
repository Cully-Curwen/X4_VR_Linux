// What every part of the x4vr command needs: where the mod's files are, reading and writing files,
// running a program.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace x4vr::linux_launcher {

// This executable as started (argv[0], symlinks unresolved, so a symlinked install path stays as
// given); set once by main().
extern std::filesystem::path program;

// The VR settings (stereo.txt): in $X4VR_DIR if set, else $XDG_STATE_HOME/x4vr, else ~/.local/state/x4vr.
std::filesystem::path get_settings_file();

// The mod's state folder: logs, the launch request, the 2D/VR marker. For now the same folder as
// stereo.txt; it gets its own lookup when stereo.txt moves to the config folder.
std::filesystem::path get_state_dir();

// X4's settings for 2D and for VR, kept apart (settings_swap.hpp); the marker names the config
// while X4 has its VR settings.
std::filesystem::path get_settings_marker();

// A whole file as text; empty if it can't be read.
std::string read_text(const std::filesystem::path& path);

// Replaces a file atomically (a temporary, then a rename): a crash mid-write leaves the old one,
// never half of X4's content.xml.
bool write_text(const std::filesystem::path& path, const std::string& text);

// A path as one shell word: in double quotes when it holds a space (Steam's launch option, the
// desktop entry's Exec line).
std::string quote_path(const std::filesystem::path& path);

// How run() runs a program. `detach`: in its own session, not waited for, output discarded. Else
// waited for, `input` on its standard input, output appended to `log` (none: discarded).
struct Run { bool detach = false; const std::string* input = nullptr; std::filesystem::path log; };

// Runs a program, found on PATH; true on exit status 0 (always true when detached).
bool run(const std::vector<std::string>& argv, const Run& options = {});

} // namespace x4vr::linux_launcher
