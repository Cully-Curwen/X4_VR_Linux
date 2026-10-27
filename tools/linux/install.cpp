// The mod's install and uninstall: the desktop entry, where the program and its source are, and
// removing what the mod set up (the menu's Uninstall screen and `x4vr uninstall`).

#include "install.hpp"
#include "common.hpp"
#include "hud.hpp"
#include "settings_swap.hpp"
#include "state_files.hpp"
#include "x4.hpp"
#include "x4_settings.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace x4vr::linux_launcher {

std::filesystem::path get_data_dir() { return program.parent_path().parent_path()/"share/x4vr"; }

// The installed program (cmake --install): the mod's files under the prefix this x4vr runs from
// and under ~/.local (the guide's prefix), those that exist. /nix/store is skipped: read-only,
// and a Nix install goes with the source folder's result link.
// `where`: the install folders where some were found.
static std::vector<std::filesystem::path> find_installed_files(std::vector<std::filesystem::path>* where = nullptr) {
    const char* home = std::getenv("HOME");
    const std::vector<std::filesystem::path> prefixes{program.parent_path().parent_path(), std::filesystem::path(home ? home : ".")/".local"};
    std::vector<std::filesystem::path> found;
    for (const std::filesystem::path& prefix : prefixes) {
        std::error_code error;
        const std::filesystem::path real = std::filesystem::weakly_canonical(prefix, error);
        if (error || real.string().starts_with("/nix/store/")) continue;
        for (const std::filesystem::path& lib : {std::filesystem::path(X4VR_INSTALL_LIBDIR), std::filesystem::path("lib"), std::filesystem::path("lib64")})
            for (const std::filesystem::path& file : {real/"bin/x4vr", real/"bin/x4vr-run", real/lib/"libx4vr.so",
                                                      real/"share/vulkan/explicit_layer.d/VkLayer_x4vr.json", real/"share/x4vr"})
                if (std::filesystem::exists(std::filesystem::symlink_status(file, error)) &&
                    std::find(found.begin(), found.end(), file) == found.end()) {
                    found.push_back(file);
                    if (where && std::find(where->begin(), where->end(), real) == where->end()) where->push_back(real);
                }
    }
    return found;
}

// Whether the removal `id` is ticked in `chosen`.
static bool is_chosen(const std::vector<Removal>& chosen, std::string_view id) {
    for (const Removal& r : chosen)
        if (r.id == id && r.on) return true;
    return false;
}

std::filesystem::path get_desktop_file() {
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) return std::filesystem::path(data)/"applications/x4vr.desktop";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".")/".local/share/applications/x4vr.desktop";
}

std::string install_desktop_entry() {
    const std::filesystem::path file = get_desktop_file();
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    const std::string exec = quote_path(program);
    const bool ok = write_text(file, "[Desktop Entry]\nType=Application\nName=X4 VR\n"
        "Comment=X4: Foundations in VR: checks, settings and launch\nExec="+exec+"\nTerminal=true\n"
        "Icon=steam_icon_392160\nCategories=Game;\n");
    return ok ? "Desktop entry written: "+file.string()+" (rofi -show drun: \"X4 VR\")" : "Can't write "+file.string();
}

// Where it is looked for:
// - a CMake build: the folder compiled in, if still there;
// - a Nix build (compiled in /build/source, gone): the folder holding the `result` link this
//   x4vr runs through, else the one Nix records as a garbage-collector root for this build.
std::filesystem::path find_source_dir() {
    const auto is_source = [](const std::filesystem::path& dir) { return std::filesystem::exists(dir/"linux/x4vr-run.in"); };
    if (const std::filesystem::path dir = X4VR_SOURCE_DIR; is_source(dir)) return dir;
    for (std::filesystem::path dir = program.parent_path(); dir.has_relative_path(); dir = dir.parent_path())
        if (dir.filename().string().starts_with("result") && is_source(dir.parent_path())) return dir.parent_path();

    std::error_code error;
    const std::filesystem::path store = std::filesystem::canonical(program, error).parent_path().parent_path(); // /nix/store/<hash>-x4vr
    if (error || !store.string().starts_with("/nix/store/")) return {};
    for (const std::filesystem::directory_entry& root : std::filesystem::directory_iterator("/nix/var/nix/gcroots/auto", error)) {
        std::error_code link_error;
        const std::filesystem::path link = std::filesystem::read_symlink(root.path(), link_error); // .../source/result
        if (!link_error && std::filesystem::canonical(link, link_error) == store && is_source(link.parent_path())) return link.parent_path();
    }
    return {};
}

std::vector<Removal> make_removals() {
    std::vector<Removal> list{
        {"desktop", "Remove the desktop entry", true, get_desktop_file().string()},
        {"hud", "Remove the HUD distance extension", true, "X4 must be closed. Saves made with it stay flagged as modified."},
        {"restore-x4", "Restore X4's settings from before the mod", false, "config.xml.x4vr-backup, the first copy: also undoes 2D settings changed since."},
        {"x4-copies", "Delete the mod's copies of X4's settings", true, "config.xml.x4vr-2d, -vr and -backup. Your 2D settings stay in config.xml."},
        {"state", "Delete the mod's settings, profiles and logs", true, get_state_dir().string()},
    };

    // The program files, ticked only when some were found.
    std::vector<std::filesystem::path> places;
    const std::vector<std::filesystem::path> files = find_installed_files(&places);
    std::string where;
    for (const std::filesystem::path& place : places) where += (where.empty() ? "" : ", ")+place.string();
    list.push_back({"program", "Remove the installed program files", !files.empty(),
        files.empty() ? "None found outside /nix/store (a Nix install goes with the GitHub clone directory)."
                      : "x4vr, x4vr-run, the mod and its data ("+std::to_string(files.size())+" found) under "+where+"."});
    return list;
}

Uninstalled uninstall(const std::vector<Removal>& chosen) {
    std::vector<std::string> done;
    std::error_code error;
    // X4 rewrites config.xml when it exits, and a VR session's 2D settings are only in the -2d copy.
    if (is_x4_running()) return {false, {"X4 is running: close it first, then uninstall."}};
    if (is_chosen(chosen, "desktop")) {
        std::filesystem::remove(get_desktop_file(), error);
        done.push_back("Desktop entry removed.");
    }

    // The HUD extension: in place, set aside for 2D, and X4's own record of it.
    if (is_chosen(chosen, "hud")) {
        if (const std::filesystem::path game = find_x4_dir(); !game.empty()) {
            std::filesystem::remove_all(hud_extension(game), error);
            std::filesystem::remove_all(get_parked_hud(game), error);
        }
        if (const std::filesystem::path config = x4_config(); !config.empty()) {
            const std::filesystem::path content = config.parent_path()/"content.xml";
            const std::string text = read_text(content);
            const std::string cleaned = std::regex_replace(text, std::regex(R"re([ \t]*<extension\s+id="x4vr_hud"[^>]*/>[ \t]*\r?\n?)re"), "");
            if (cleaned != text) write_text(content, cleaned);
        }
        done.push_back("HUD distance extension removed.");
    }

    // A VR session that didn't finish: its 2D settings back in place first (only then: the -2d copy
    // is otherwise an old one, and the live config.xml has the player's 2D settings).
    if (!switch_x4_settings(false).ok) {
        done.push_back("Can't put X4's 2D settings back (see x4vr settings-mode 2d): its copies and the mod's settings are kept.");
        return {false, done};
    }
    if (const std::filesystem::path config = x4_config(); !config.empty()) {
        const std::string backup = config.string()+".x4vr-backup";
        if (is_chosen(chosen, "restore-x4") && std::filesystem::exists(backup))
            done.push_back(x4vr::linux_port::copy_over(backup, config) ? "X4's settings restored from before the mod." : "Can't restore X4's settings.");
        if (is_chosen(chosen, "x4-copies")) {
            for (const char* suffix : {".x4vr-backup", ".x4vr-2d", ".x4vr-vr"}) std::filesystem::remove(config.string()+suffix, error);
            done.push_back("The mod's copies of X4's settings deleted.");
        }
    }

    // The mod's own files in its state folder, and the program files.
    if (is_chosen(chosen, "state")) {
        x4vr::linux_port::remove_state_files(get_state_dir()); // its own files only: X4VR_DIR may be any directory
        done.push_back("Mod settings and logs deleted.");
    }
    if (is_chosen(chosen, "program")) {
        int removed = 0;
        for (const std::filesystem::path& file : find_installed_files()) removed += int(std::filesystem::remove_all(file, error) > 0);
        done.push_back("Program files removed: "+std::to_string(removed)+".");
    }
    return {true, done};
}

} // namespace x4vr::linux_launcher
