// The mod's install and uninstall: the desktop entry, where the program and its source are, and
// removing what the mod set up (the menu's Uninstall screen and `x4vr uninstall`).

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace x4vr::linux_launcher {

// The desktop entry: $XDG_DATA_HOME (default ~/.local/share)/applications/x4vr.desktop.
std::filesystem::path get_desktop_file();

// Writes the desktop entry ("X4 VR" in the app menu, running this x4vr in a terminal). What it did,
// in a line for the menu.
std::string install_desktop_entry();

// The source folder this was built from (the GitHub clone), if it can be found; shown at uninstall,
// never deleted.
std::filesystem::path find_source_dir();

// One thing uninstall can remove: `id` as typed after `x4vr uninstall`, `on` ticked in the menu.
struct Removal {
    std::string id, label;
    bool on;
    std::string help;
};

// What the mod set up that uninstall can remove, each with its default tick and what it removes.
std::vector<Removal> make_removals();

// What uninstall() did, a line each; not ok when it stopped early (X4 running, 2D settings not back).
struct Uninstalled {
    bool ok = true;
    std::vector<std::string> done;
};

// Removes the ticked ones (`on`), nothing while X4 runs.
Uninstalled uninstall(const std::vector<Removal>& chosen);

} // namespace x4vr::linux_launcher
