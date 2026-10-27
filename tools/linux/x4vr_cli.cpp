// x4vr: the Linux port's tool. Without arguments, a full-screen menu (checks, VR settings, Launch
// in VR, HUD distance, bug report, uninstall). Subcommands: the steps x4vr-run takes, plus a few
// for when the menu can't help (x4vr help).
#include "elf_classes.hpp"
#include "code_scan.hpp"
#include "../launcher/hud_mod.hpp"
#include "../launcher/launcher_settings.hpp"
#include "build_info.h"
#include "gpu_status.hpp"
#include "launch_option.hpp"
#include "settings_control.hpp"
#include "settings_swap.hpp"
#include "state_files.hpp"
#include "steam_config.hpp"
#include "terminal_ui.hpp"
#include "common.hpp"
#include "x4.hpp"
#include "hud.hpp"
#include "launch.hpp"
#include "report.hpp"
#include "status.hpp"
#include "x4_settings.hpp"
#include "x4_settings_rules.hpp"
#include "vr_query.hpp"
#include <vulkan/vulkan.h>
#include <openvr.h>
#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace x4vr::linux_launcher {
namespace {

void usage() {
    std::cout <<
        "Usage: x4vr [command]\n"
        "\n"
        "  (no command) or menu\n"
        "      The menu: checks, VR settings, Launch X4 in VR, HUD distance, bug report, uninstall.\n"
        "  uninstall [desktop | hud | restore-x4 | x4-copies | state | program]...\n"
        "      Removes what the mod set up, only what you name. x4vr uninstall alone lists what each\n"
        "      name removes, removing nothing. The menu's Uninstall screen does the same.\n"
        "  version\n"
        "      The version and commit this x4vr was built from (its date, local changes) and the ones before it.\n"

        "  vr-vulkan\n"
        "      For x4vr-run: asks SteamVR for its Vulkan needs before X4 starts (instance extensions,\n"
        "      headset GPU, device extensions), one per line.\n"
        "  report\n"
        "      Packs logs (the mod's and SteamVR's), settings, versions and a summary into\n"
        "      ~/x4vr-report-<time>.tar.gz for a bug report. Made while X4 hangs at its start, it also\n"
        "      records what X4's threads wait on (a backtrace with gdb or eu-stack).\n"
        "  patterns [path to the X4 executable]\n"
        "      Finds the X4 code the mod patches and hooks, as the mod does at startup, and prints where.\n"
        "  ctl recenter | flat\n"
        "      While X4 runs in VR: recentre, or switch the flat screen (for desktop key bindings).\n"
        "\n"
        "Used by x4vr-run before and after X4 runs:\n"
        "  settings-mode vr | 2d | status   X4's VR settings in place, or its 2D ones back\n"
        "  fix-settings [--auto]            X4's VR settings fixed for VR (backup: config.xml.x4vr-backup)\n"
        "  hud --refresh                    the HUD extension as chosen in the menu, rebuilt after a game update\n"
        "  hud --park                       the HUD extension out of X4's sight, for 2D\n";
}

// ---- ctl ------------------------------------------------------------------------------------
int ctl(const std::vector<std::string_view>& args) {
    if (args.size() != 1 || (args[0] != "recenter" && args[0] != "flat")) { usage(); return 2; }
    const auto path = get_settings_file();
    const int next = x4vr::linux_port::control_settings(path, args[0]);
    if (next < 0) { std::cerr << "Can't update " << path << " (start X4 once with x4vr-run)\n"; return 1; }
    std::cout << (args[0] == "recenter" ? "recentred" : next == 2 ? "flat screen on" : "flat screen automatic") << '\n';
    return 0;
}

// ---- menu (x4vr without arguments): checks, settings, launch in VR, bug report, uninstall ----
std::filesystem::path desktop_file() {
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    return (data && *data ? std::filesystem::path(data) : std::filesystem::path(home ? home : ".")/".local/share")/"applications/x4vr.desktop";
}

// Copies text to the clipboard: wl-copy (Wayland), xclip or xsel (X11), else the terminal's
// own clipboard (OSC 52, which most terminals support). Returns how.
std::string copy_to_clipboard(const std::string& text) {
    for (const auto& tool : std::vector<std::vector<std::string>>{{"wl-copy"}, {"xclip", "-selection", "clipboard"}, {"xsel", "--clipboard", "--input"}})
        if (run(tool, {.input = &text})) return tool[0];
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (size_t i = 0; i < text.size(); i += 3) {
        const uint32_t n = (uint32_t(uint8_t(text[i])) << 16) | (i+1 < text.size() ? uint32_t(uint8_t(text[i+1])) << 8 : 0) |
                           (i+2 < text.size() ? uint8_t(text[i+2]) : 0);
        encoded += table[(n >> 18) & 63]; encoded += table[(n >> 12) & 63];
        encoded += i+1 < text.size() ? table[(n >> 6) & 63] : '=';
        encoded += i+2 < text.size() ? table[n & 63] : '=';
    }
    const std::string sequence = "\x1b]52;c;"+encoded+"\a";
    (void)!::write(STDOUT_FILENO, sequence.data(), sequence.size());
    return "the terminal";
}
std::string launch_option_steps() { return "In Steam: X4 > Properties > General > Launch options, paste it there."; }

using x4vr::linux_port::copy_over;
int cmd_settings_mode(const std::vector<std::string_view>& args) {
    if (args.size() != 1 || (args[0] != "vr" && args[0] != "2d" && args[0] != "status")) { usage(); return 2; }

    if (args[0] == "status") {
        const std::filesystem::path in_vr = x4vr::linux_port::config_in_vr(get_settings_marker());
        const std::filesystem::path config = in_vr.empty() ? x4_config() : in_vr;
        std::cout << (!in_vr.empty() ? "X4 has its VR settings (" : "X4 has its 2D settings (") << config.string() << ")\n";
        return 0;
    }

    const x4vr::linux_port::Switched done = switch_x4_settings(args[0] == "vr");
    if (!done.ok) { std::cerr << "x4vr: " << done.message << '\n'; return 1; }
    if (done.changed) std::cout << "x4vr: " << done.message << '\n';
    return 0;
}
std::string install_desktop() {
    const auto file = desktop_file();
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    const auto exec = quote_path(program);
    const bool ok = write_text(file, "[Desktop Entry]\nType=Application\nName=X4 VR\n"
        "Comment=X4: Foundations in VR: checks, settings and launch\nExec="+exec+"\nTerminal=true\n"
        "Icon=steam_icon_392160\nCategories=Game;\n");
    return ok ? "Desktop entry written: "+file.string()+" (rofi -show drun: \"X4 VR\")" : "Can't write "+file.string();
}

// `x4vr vr-vulkan`, run by x4vr-run just before X4 starts: SteamVR's Vulkan requirements for the mod
// (src/linux/vr_query.hpp), one per line on stdout, progress on stderr (x4vr.log). SteamVR may create
// a Vulkan instance of its own to answer, which inside X4's vkCreateInstance or vkCreateDevice hangs
// X4 (issue #7); in this process it's harmless. Starts SteamVR if it isn't running (X4VR_ALWAYS=1
// and Steam's Play), as the mod's own VR_Init did. Waits for SteamVR and the headset
// (X4VR_HEADSET_WAIT seconds, default 120): a Steam Frame connects some seconds after SteamVR.
int vr_vulkan() {
    using namespace std::chrono;
    const auto fail = [](const std::string& why) { std::cerr << "x4vr: SteamVR's Vulkan needs not read: " << why << "\n"; return 1; };
    const char* wait_text = std::getenv("X4VR_HEADSET_WAIT");
    const int wait = wait_text && *wait_text ? std::atoi(wait_text) : 120;
    const auto deadline = steady_clock::now()+seconds(std::max(wait, 0));
    // Asked as a background app, which doesn't show up as a running game, but doesn't start SteamVR
    // either: started here, as the menu does.
    if (!is_steamvr_running()) {
        std::cerr << "x4vr: SteamVR isn't running: starting it\n";
        run({"steam", "steam://rungameid/250820"}, {.detach = true});
    }
    for (bool waiting = false;;) {
        auto error = vr::VRInitError_None;
        if (vr::VR_Init(&error, vr::VRApplication_Background) && error == vr::VRInitError_None) break;
        // Not ready yet: SteamVR still starting (its vrserver process can exist before it takes
        // apps: the menu's Launch only waits for the process), or no headset yet.
        const bool not_ready = error == vr::VRInitError_Init_NoServerForBackgroundApp || error == vr::VRInitError_Init_HmdNotFound ||
                               error == vr::VRInitError_Init_HmdNotFoundPresenceFailed || error == vr::VRInitError_Driver_WirelessHmdNotConnected;
        if (!not_ready || steady_clock::now() >= deadline) return fail(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        if (!waiting) std::cerr << "x4vr: waiting up to " << wait << " s for SteamVR and the headset: " << vr::VR_GetVRInitErrorAsEnglishDescription(error) << "\n";
        waiting = true;
        std::this_thread::sleep_for(seconds(1));
    }
    struct Shutdown { ~Shutdown() { vr::VR_Shutdown(); } } shutdown;
    auto* system = vr::VRSystem();
    auto* compositor = vr::VRCompositor();
    if (!system || !compositor) return fail("no SteamVR compositor");
    const auto query = [](auto ask) {
        const uint32_t size = ask(nullptr, 0);
        std::string text(size, '\0');
        if (size) ask(text.data(), size);
        return x4vr::vr_query::words(text.c_str());
    };
    const auto instance_extensions = query([&](char* text, uint32_t size) { return compositor->GetVulkanInstanceExtensionsRequired(text, size); });
    if (instance_extensions.empty()) return fail("SteamVR named no Vulkan instance extensions");

    // The headset's GPU: SteamVR names it among a Vulkan instance's devices, so make one.
    void* vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) return fail("no Vulkan loader (libvulkan.so.1)");
    struct Close { void* lib; ~Close() { dlclose(lib); } } close_vulkan{vulkan};
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan, "vkGetInstanceProcAddr"));
    const auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    const auto available = gipa ? reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties")) : nullptr;
    if (!create || !available) return fail("Vulkan loader incomplete");
    uint32_t count = 0;
    available(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    available(nullptr, &count, properties.data());
    std::vector<const char*> enabled; // SteamVR's that this loader has
    for (const auto& name : instance_extensions)
        for (const auto& p : properties) if (name == p.extensionName) { enabled.push_back(name.c_str()); break; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "x4vr"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size()); info.ppEnabledExtensionNames = enabled.data();
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&info, nullptr, &instance) != VK_SUCCESS) return fail("can't create a Vulkan instance");
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    struct Destroy { VkInstance instance; PFN_vkDestroyInstance destroy; ~Destroy() { if (destroy) destroy(instance, nullptr); } } destroy_instance{instance, destroy};
    uint64_t device = 0;
    system->GetOutputDevice(&device, vr::TextureType_Vulkan, instance);
    if (!device) return fail("SteamVR named no headset GPU");
    const auto physical = reinterpret_cast<VkPhysicalDevice>(device);
    const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    if (!properties2) return fail("Vulkan 1.1 not available");
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 gpu{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    gpu.pNext = &id;
    properties2(physical, &gpu);
    const auto device_extensions = query([&](char* text, uint32_t size) { return compositor->GetVulkanDeviceExtensionsRequired(physical, text, size); });
    std::cerr << "x4vr: SteamVR's Vulkan needs read: headset GPU " << gpu.properties.deviceName << ", "
              << instance_extensions.size() << " instance and " << device_extensions.size() << " device extensions\n";
    std::cout << x4vr::vr_query::join(instance_extensions) << "\n" << x4vr::vr_query::uuid_text(id.deviceUUID) << "\n"
              << x4vr::vr_query::join(device_extensions) << "\n";
    return 0;
}

constexpr const char* issues_url = "https://github.com/ToffelsKater/X4_VR/issues"; // to find one to add to, or open a new one

// Uninstall: what the mod left, each as a choice.
// The installed program (cmake --install): the mod's files under the prefix this x4vr runs from
// and under ~/.local (the guide's prefix), those that exist. /nix/store is skipped: read-only,
// and a Nix install goes with the source folder's result link.
// `where`: the install folders where some were found.
std::vector<std::filesystem::path> installed_files(std::vector<std::filesystem::path>* where = nullptr) {
    const char* home = std::getenv("HOME");
    std::vector<std::filesystem::path> prefixes{program.parent_path().parent_path(), std::filesystem::path(home ? home : ".")/".local"};
    std::vector<std::filesystem::path> found;
    for (const auto& prefix : prefixes) {
        std::error_code error;
        const auto real = std::filesystem::weakly_canonical(prefix, error);
        if (error || real.string().rfind("/nix/store/", 0) == 0) continue;
        for (const std::filesystem::path& lib : {std::filesystem::path(X4VR_INSTALL_LIBDIR), std::filesystem::path("lib"), std::filesystem::path("lib64")})
            for (const auto& file : {real/"bin/x4vr", real/"bin/x4vr-run", real/lib/"libx4vr.so",
                                     real/"share/vulkan/explicit_layer.d/VkLayer_x4vr.json", real/"share/x4vr"})
                if (std::filesystem::exists(std::filesystem::symlink_status(file, error)) &&
                    std::find(found.begin(), found.end(), file) == found.end()) {
                    found.push_back(file);
                    if (where && std::find(where->begin(), where->end(), real) == where->end()) where->push_back(real);
                }
    }
    return found;
}
// The source folder this was built from (the GitHub clone), if it can be found:
// - a CMake build: the folder compiled in, if still there;
// - a Nix build (compiled in /build/source, gone): the folder holding the `result` link this
//   x4vr runs through, else the one Nix records as a garbage-collector root for this build.
std::filesystem::path source_dir() {
    const auto is_source = [](const std::filesystem::path& dir) { return std::filesystem::exists(dir/"linux/x4vr-run.in"); };
    if (const std::filesystem::path dir = X4VR_SOURCE_DIR; is_source(dir)) return dir;
    for (auto dir = program.parent_path(); dir.has_relative_path(); dir = dir.parent_path())
        if (dir.filename().string().rfind("result", 0) == 0 && is_source(dir.parent_path())) return dir.parent_path();
    std::error_code error;
    const auto store = std::filesystem::canonical(program, error).parent_path().parent_path(); // /nix/store/<hash>-x4vr
    if (error || store.string().rfind("/nix/store/", 0) != 0) return {};
    for (const auto& root : std::filesystem::directory_iterator("/nix/var/nix/gcroots/auto", error)) {
        std::error_code link_error;
        const auto link = std::filesystem::read_symlink(root.path(), link_error); // .../source/result
        if (!link_error && std::filesystem::canonical(link, link_error) == store && is_source(link.parent_path())) return link.parent_path();
    }
    return {};
}
struct Removal { std::string id, label; bool on; std::string help; };
std::vector<Removal> removals() {
    std::vector<Removal> list{
        {"desktop", "Remove the desktop entry", true, desktop_file().string()},
        {"hud", "Remove the HUD distance extension", true, "X4 must be closed. Saves made with it stay flagged as modified."},
        {"restore-x4", "Restore X4's settings from before the mod", false, "config.xml.x4vr-backup, the first copy: also undoes 2D settings changed since."},
        {"x4-copies", "Delete the mod's copies of X4's settings", true, "config.xml.x4vr-2d, -vr and -backup. Your 2D settings stay in config.xml."},
        {"state", "Delete the mod's settings, profiles and logs", true, get_state_dir().string()},
    };
    std::vector<std::filesystem::path> places;
    const auto files = installed_files(&places);
    std::string where;
    for (const auto& p : places) where += (where.empty() ? "" : ", ")+p.string();
    list.push_back({"program", "Remove the installed program files", !files.empty(),
        files.empty() ? "None found outside /nix/store (a Nix install goes with the GitHub clone directory)."
                      : "x4vr, x4vr-run, the mod and its data ("+std::to_string(files.size())+" found) under "+where+"."});
    return list;
}
// What uninstall() did, a line each; not ok when it stopped early (X4 running, 2D settings not back).
struct Uninstalled {
    bool ok = true;
    std::vector<std::string> done;
};
Uninstalled uninstall(const std::vector<Removal>& chosen) {
    std::vector<std::string> done;
    const auto on = [&](std::string_view id) { return std::any_of(chosen.begin(), chosen.end(), [&](const auto& r) { return r.id == id && r.on; }); };
    std::error_code error;
    // X4 rewrites config.xml when it exits, and a VR session's 2D settings are only in the -2d copy.
    if (is_x4_running()) return {false, {"X4 is running: close it first, then uninstall."}};
    if (on("desktop")) { std::filesystem::remove(desktop_file(), error); done.push_back("Desktop entry removed."); }
    {
        if (on("hud")) {
            if (const auto game = find_x4_dir(); !game.empty()) { // in place, and aside for 2D
                std::filesystem::remove_all(hud_extension(game), error);
                std::filesystem::remove_all(get_parked_hud(game), error);
            }
            if (const auto config = x4_config(); !config.empty()) { // and X4's own record of it
                const auto content = config.parent_path()/"content.xml";
                const auto text = read_text(content);
                const auto cleaned = std::regex_replace(text, std::regex(R"re([ \t]*<extension\s+id="x4vr_hud"[^>]*/>[ \t]*\r?\n?)re"), "");
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
        if (const auto config = x4_config(); !config.empty()) {
            const auto backup = config.string()+".x4vr-backup";
            if (on("restore-x4") && std::filesystem::exists(backup))
                done.push_back(copy_over(backup, config) ? "X4's settings restored from before the mod." : "Can't restore X4's settings.");
            if (on("x4-copies")) {
                for (const auto* suffix : {".x4vr-backup", ".x4vr-2d", ".x4vr-vr"}) std::filesystem::remove(config.string()+suffix, error);
                done.push_back("The mod's copies of X4's settings deleted.");
            }
        }
    }
    if (on("state")) {
        x4vr::linux_port::remove_state_files(get_state_dir()); // its own files only: X4VR_DIR may be any directory
        done.push_back("Mod settings and logs deleted.");
    }
    if (on("program")) {
        int removed = 0;
        for (const auto& file : installed_files()) removed += int(std::filesystem::remove_all(file, error) > 0);
        done.push_back("Program files removed: "+std::to_string(removed)+".");
    }
    return {true, done};
}

// `x4vr uninstall [name]...`: removes only what is named (no "all": each removal is chosen); alone,
// lists the names and what each removes, removing nothing. As the menu's Uninstall screen.
int cmd_uninstall(const std::vector<std::string_view>& args) {
    std::vector<Removal> chosen = removals();
    if (args.empty()) {
        std::cout << "x4vr uninstall <name>... removes what you name:\n";
        for (const Removal& r : chosen) std::printf("  %-11s %s: %s\n", r.id.c_str(), r.label.c_str(), r.help.c_str());
        return 0;
    }

    for (Removal& r : chosen) r.on = false;
    for (const std::string_view name : args) {
        bool known = false;
        for (Removal& r : chosen) {
            if (r.id != name) continue;
            r.on = true;
            known = true;
        }
        if (!known) { usage(); return 2; }
    }

    const Uninstalled result = uninstall(chosen);
    for (const std::string& line : result.done) (result.ok ? std::cout : std::cerr) << line << '\n';
    if (!result.ok) return 1;

    // What only the player can do (the menu screen's "Then, by hand").
    const std::filesystem::path source = source_dir();
    std::cout << "Then, by hand: clear X4's launch option in Steam (X4 > Properties > General > Launch options)"
              << (source.empty() ? std::string(".") : "; delete the GitHub clone directory: "+source.string()) << '\n';
    return 0;
}

// ---- profiles: sets of VR settings ------------------------------------------------------------
// Built in: share/x4vr/profiles/<name>.txt (installed); the player's own: <state>/profiles/.
// A profile holds stereo.txt lines for the settings the menu shows; loading one writes them to
// stereo.txt. The profile last loaded or saved is named in <state>/profile.
std::filesystem::path data_dir() { return program.parent_path().parent_path()/"share/x4vr"; }
std::filesystem::path user_profiles() { return get_state_dir()/"profiles"; }
// The settings a profile holds, with the mod's defaults (runtime_bootstrap.hpp) for missing lines.
const std::vector<std::pair<const char*, const char*>> profile_keys{
    {"stereo", "1"}, {"shared_pose", "1"}, {"ipd_scale", "1"}, {"predict", "0.035"}, {"async_submit", "1"}, {"cursor", "1"},
    {"theater", "1"}, {"theater_distance", "2"}, {"theater_width", "2.2"}, {"x4_width", "0"}, {"x4_height", "0"}, {"hud_factor", ""}};
struct Profile { std::string name; bool built_in; std::filesystem::path file; };
std::vector<Profile> profiles() {
    std::vector<Profile> list;
    for (const bool built_in : {true, false}) {
        std::vector<Profile> found;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(built_in ? data_dir()/"profiles" : user_profiles(), error))
            if (entry.path().extension() == ".txt") found.push_back({entry.path().stem().string(), built_in, entry.path()});
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        for (auto& p : found)
            if (std::none_of(list.begin(), list.end(), [&](const auto& q) { return q.name == p.name; })) list.push_back(std::move(p));
    }
    return list;
}
std::string current_profile() {
    auto name = read_text(get_state_dir()/"profile");
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    return name;
}
// Whether stereo.txt has what the profile sets (numbers compared as numbers).
bool profile_matches(const Profile& p) {
    for (const auto& [key, fallback] : profile_keys) {
        const auto wanted = x4vr::linux_port::read_setting(p.file, key, "");
        if (wanted.empty()) continue;
        const auto now = x4vr::linux_port::read_setting(get_settings_file(), key, fallback);
        if (now.empty() || std::fabs(std::atof(now.c_str())-std::atof(wanted.c_str())) > 1e-6) return false;
    }
    return true;
}
bool load_profile(const Profile& p) {
    bool ok = true;
    for (const auto& [key, fallback] : profile_keys)
        if (const auto value = x4vr::linux_port::read_setting(p.file, key, ""); !value.empty())
            ok = x4vr::linux_port::write_setting(get_settings_file(), key, value) && ok;
    return write_text(get_state_dir()/"profile", p.name+"\n") && ok;
}
std::string save_profile(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name[0] == '.') return "Not a usable name: "+name;
    for (const auto& p : profiles()) if (p.built_in && p.name == name) return "\""+name+"\" is built in: pick another name.";
    std::error_code error;
    std::filesystem::create_directories(user_profiles(), error);
    std::string text = "# x4vr profile, saved from the menu\n";
    for (const auto& [key, fallback] : profile_keys)
        if (const auto value = x4vr::linux_port::read_setting(get_settings_file(), key, fallback); !value.empty()) text += std::string(key)+"="+value+"\n";
    if (!write_text(user_profiles()/(name+".txt"), text)) return "Can't write "+(user_profiles()/(name+".txt")).string();
    write_text(get_state_dir()/"profile", name+"\n");
    return "Saved as profile \""+name+"\".";
}

// ---- notices: known issues, tips and requests for help, shipped in share/x4vr/notices.txt -----
// One per line, "issue: text", "tip: text" or "help: text"; # starts a comment.
std::vector<std::pair<int, std::string>> notices() {
    std::vector<std::pair<int, std::string>> list;
    std::ifstream in(data_dir()/"notices.txt");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const int kind = line.rfind("tip:", 0) == 0 ? 0 : line.rfind("issue:", 0) == 0 ? 1 : line.rfind("help:", 0) == 0 ? 2 : -1;
        if (kind < 0) continue;
        auto text = line.substr(line.find(':')+1);
        text.erase(0, text.find_first_not_of(' '));
        list.emplace_back(kind, text);
    }
    return list;
}

// The headset's frame rate while X4 runs in VR: the mod's newest pair_stats.txt line (every ~2 s).
std::optional<x4vr::launcher::Stats> headset_stats() {
    const auto path = get_state_dir()/"pair_stats.txt";
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error || std::filesystem::file_time_type::clock::now()-time > std::chrono::seconds(6)) return std::nullopt;
    std::ifstream in(path);
    std::string line, last;
    while (std::getline(in, line)) if (!line.empty()) last = line;
    x4vr::launcher::Stats stats{};
    if (!x4vr::launcher::parse_stats_line(last, stats)) return std::nullopt;
    return stats;
}

// ---- tiling window managers: rules that float the VR window ------------------------------------
// X4 renders at its window's size, and a tiling window manager resizes the window (and honours
// X4's fullscreen request, which shrinks it to the monitor). In VR the window's class is X4VR
// (x4vr-run sets SDL_APP_ID), so a rule can float it and leave 2D (class X4) alone.
struct TilingRule { std::string id, name, where, rule; bool tested; };
std::vector<TilingRule> tiling_rules() {
    return {
        {"hyprland_lua", "Hyprland 0.55+ (Lua config)", "~/.config/hypr/hyprland.lua",
         "hl.window_rule({ name = \"x4-vr\", match = { class = \"^(X4VR)$\" }, float = true, "
         "suppress_event = \"fullscreen maximize\", fullscreen_state = \"0 0\", confine_pointer = true })", true},
        {"hyprland_conf", "Hyprland (hyprland.conf)", "~/.config/hypr/hyprland.conf",
         "windowrule = float, class:^(X4VR)$\nwindowrule = suppressevent fullscreen maximize, class:^(X4VR)$", false},
        {"sway", "Sway", "~/.config/sway/config", "for_window [class=\"X4VR\"] floating enable, fullscreen disable", false},
        {"i3", "i3", "~/.config/i3/config", "for_window [class=\"X4VR\"] floating enable, fullscreen disable", false},
    };
}
// The tiling window manager this session runs, if one of those: "hyprland", "sway", "i3" or "".
std::string tiling_wm() {
    if (const char* h = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); h && *h) return "hyprland";
    if (const char* s = std::getenv("SWAYSOCK"); s && *s) return "sway";
    if (const char* i = std::getenv("I3SOCK"); i && *i) return "i3";
    return {};
}
void tiling_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    Menu menu(terminal);
    menu.title = "X4 VR  ·  Tiling window manager rules";
    menu.keys = "↑↓ move   Enter copy   Esc back";
    menu.label_width = 60;
    const auto wm = tiling_wm();
    const auto rules = tiling_rules();
    for (;;) {
        std::vector<Item> items{
            info("X4 renders at its window's size. A tiling window manager resizes it (or makes it fullscreen at the monitor's "
                 "size), so the headset gets a smaller image. In VR the window's class is X4VR (2D keeps X4), so a rule can float "
                 "only the VR window at the size the mod sets. Add the rule for yours to its config; it reloads on save. "
                 "Hyprland's Lua rule also keeps the cursor inside X4's window (confine_pointer), so moving the mouse "
                 "can't move focus away and stop mouse look on foot; keybinds still switch windows."),
        };
        for (const auto& r : rules) {
            const bool here = r.id.rfind(wm.empty() ? "-" : wm, 0) == 0;
            items.push_back(section(r.name+(here ? "  (this session)" : "")+(r.tested ? "" : "  (untested)")));
            items.push_back(info("In "+r.where+":"));
            std::istringstream lines(r.rule);
            for (std::string line; std::getline(lines, line);) items.push_back(info("  "+line));
            items.push_back(action(r.id, "Copy this rule", "Copies the rule above to the clipboard."));
        }
        items.push_back(section("Check"));
        items.push_back(info("Launch in VR, then hyprctl clients (Hyprland) or swaymsg -t get_tree (Sway): the X4VR window should be "
                             "floating, not fullscreen, at the VR resolution. Remove the rule when uninstalling."));
        items.push_back(action("back", "Back"));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || e.id == "back") return;
        if (e.kind != Event::Activate) continue;
        for (const auto& r : rules)
            if (r.id == e.id) menu.messages = {"Copied ("+copy_to_clipboard(r.rule)+"): paste it into "+r.where+"."};
    }
}

void uninstall_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    auto chosen = removals();
    Menu menu(terminal);
    menu.title = "X4 VR  ·  Uninstall";
    menu.keys = "↑↓ move   Space tick   Enter select   Esc back";
    menu.label_width = 48;
    for (;;) {
        std::vector<Item> items{section("Remove what the mod set up")};
        for (const auto& r : chosen) items.push_back(toggle(r.id, r.label, r.on, r.help, {}));
        items.push_back(action("go", "Remove the ticked items", "Your 2D settings and saves stay."));
        items.push_back(action("back", "Back"));
        items.push_back(section("Then, by hand"));
        items.push_back(info("1. In Steam: X4 > Properties > General > Launch options: clear the line."));
        const auto source = source_dir();
        items.push_back(info("2. Delete the GitHub clone directory"+(source.empty() ? std::string(" you built the mod from.")
                                                                                  : ": "+source.string())));
        if (const auto wm = tiling_wm(); !wm.empty())
            items.push_back(info("3. Remove the X4VR window rule from your "+std::string(wm == "hyprland" ? "Hyprland" : wm == "sway" ? "Sway" : "i3")
                                 +" config, if you added one "
                                 "(Setup > Tiling window manager rules shows it)."));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || (e.kind == Event::Activate && e.id == "back")) return;
        if (e.kind == Event::Changed)
            for (auto& r : chosen) if (r.id == e.id) r.on = e.item.on;
        if (e.kind == Event::Activate && e.id == "go") menu.messages = uninstall(chosen).done;
    }
}

// README "Set X4's options" as a screen, by where each setting is in X4, with the value X4 has
// in its VR settings (config.xml during a VR session, else the VR copy). Linux: windowed, not fullscreen + DSR.
void checklist_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    Menu menu(terminal);
    menu.title = "X4 VR  ·  In-game settings checklist";
    menu.keys = "↑↓ scroll   Esc back";
    menu.timeout_ms = 2000;
    menu.label_width = 38;
    for (;;) {
        const auto config = x4_config();
        const bool in_vr = std::filesystem::exists(get_settings_marker());
        const auto vr_copy = std::filesystem::path(config.string()+".x4vr-vr");
        const bool have = !config.empty() && (in_vr || std::filesystem::exists(vr_copy));
        const auto xml = have ? read_text(in_vr ? config : vr_copy) : std::string();
        const auto checks = have ? x4_checks(xml) : std::vector<x4vr::launcher::Check>{};
        // One row: from X4's settings file when it has the key, else to check in the game.
        std::vector<Item> items;
        const auto row = [&](const char* label, const char* want, std::initializer_list<const char*> keys) {
            using x4vr::linux_port::Kind;
            for (const auto& check : checks)
                for (const auto* key : keys)
                    if (!check.fix.empty() && check.fix[0].first == key) {
                        const auto wanted = *want ? std::string(want) : check.label.substr(check.label.rfind(' ')+1);
                        const auto kind = x4vr::linux_port::kind(check);
                        items.push_back(status(label, check.ok ? 0 : kind == Kind::required ? 2 : 1,
                            wanted+(check.ok ? "" : "   now: "+check.current+(kind == Kind::required ? "" : kind == Kind::optional ? " (optional)"
                                                                             : ": "+std::string(x4vr::linux_port::frame_limit_reason)))));
                        if (!check.ok && kind != Kind::required) items.back().mark = kind == Kind::optional ? "·" : "!";
                        return;
                    }
            items.push_back(status(label, 3, std::string(*want ? want : "the VR resolution")+(have || keys.size() == 0 ? "" : "   (not known yet)")));
            items.back().mark = "-";
        };
        items.push_back(info(have ? "Key:  ✓ right   ✗ wrong: set at the next VR launch   · optional   ! worth a look   "
                                    "- check by hand in X4 (the mod can't read it)"
                                  : "X4's VR settings are made at the first VR launch; until then check everything by hand in X4."));
        items.push_back(section("Settings > Controls > Head Tracking Support"));
        row("OpenTrack Support", "On", {"enableopentrack"});
        items.push_back(section("Settings > Controls > OpenTrack"));
        row("Head Rotation Factor", "100 %", {"opentrackanglefactor"});
        row("Head Position Factor", "100 %", {"opentrackpositionfactor"});
        items.push_back(section("Settings > Display"));
        row("Display Mode", "Windowed", {"fullscreen", "borderless"});
        row("Resolution", "", {"res_width"}); // the size checked (menu choice or automatic)
        row("FOV", "maximum (120°)", {"fov"});
        row("Anti-Aliasing", "None or not temporal", {"antialiasing"});
        row("Upscaling (AMD FSR, DLSS)", "Off", {"upmode"});
        row("AMD FSR frame generation", "Off", {"fsr3g"});
        row("DLSS frame generation", "Off", {"dlssg"});
        row("VSync", "Off", {"presentmode"});
        row("Frame Rate Limit", "Off", {"frameratelimit"});
        items.push_back(section("Settings > Graphics (recommended, not required)"));
        row("Chromatic Aberration", "Off", {"chromaticaberration"});
        row("Distortion", "Off", {"distortion"});
        if (is_hud_scaled()) { // only matters with the HUD extension
            items.push_back(section("Settings > Extensions (HUD Scaled is on)"));
            row("Protected UI Mode", "Off", {"uisafemode"});
        }
        items.push_back(action("back", "Back"));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || e.id == "back") return;
    }
}

// The menu: one screen in sections. Status and the X4 settings list are rebuilt every 2 s.
// Settings tagged "live" are in stereo.txt, re-read by the mod every half second; "next launch"
// ones are applied by x4vr-run when X4 starts in VR (resolution, HUD extension).
int menu() {
    using namespace x4vr::tui;
    setenv("X4VR_NO_STEAMVR_QUERY", "1", 1); // the settings checks: no SteamVR client from here
    Terminal terminal;
    if (!terminal.ok()) { std::cerr << "x4vr: the menu needs a terminal (run it in one, or use the subcommands: x4vr help)\n"; return 1; }
    const auto path = get_settings_file();
    if (!std::filesystem::exists(path)) { // first use: the defaults, as x4vr-run would copy them
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::copy_file(data_dir()/"stereo.txt", path, error);
        if (const auto list = profiles(); !list.empty() && list[0].built_in) load_profile(list[0]);
    }
    Menu menu(terminal);
    menu.title = "X4 VR for Linux";
    menu.keys = "↑↓ move   Enter select   Space tick   ←→ change";
    menu.timeout_ms = 2000; // live status (SteamVR, X4, headset)
    {
        std::vector<Item> waiting{info("Checking X4...")};
        Event e;
        menu.timeout_ms = 0;
        menu.step(waiting, e);
        menu.timeout_ms = 2000;
    }
    x4vr::linux_port::code::X4Sites sites;
    bool scanned = false;
    if (const auto game = find_x4_dir(); !game.empty() && std::filesystem::exists(game/"X4")) {
        try { sites = x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load((game/"X4").string())); scanned = true; } catch (...) {}
    }
    const auto board = notices();
    const auto get = [&](const char* key, const char* fallback) { return x4vr::linux_port::read_setting(path, key, fallback); };
    const auto num = [&](const char* key, const char* fallback) { return std::atof(get(key, fallback).c_str()); };
    static constexpr std::pair<int, int> modes[] = {{1920, 1080}, {2560, 1440}, {2880, 1620}, {3200, 1800}, {3840, 2160}};
    const std::string live = "live", next = "next launch";
    bool custom_resolution = false; // "Custom" picked: width and height shown even if they match a listed size
    for (;;) {
        const Status c = read_status();
        int scan_state = 3;
        const auto scan_text = scanned ? make_scan_summary(sites, scan_state) : std::string("X4 not found (set X4VR_GAME_DIR)");
        const double wanted_hud_factor = read_hud_factor();
        const double hud_factor = wanted_hud_factor >= 0 ? wanted_hud_factor : c.hud;
        std::vector<Item> items;

        items.push_back(info("Version "+std::string(X4VR_BUILD))); // build_info.h: git describe
        items.push_back(section("Status"));
        items.push_back(status("SteamVR", c.steamvr ? 0 : 3, c.steamvr ? "running" : "not running (Launch starts it)"));
        items.push_back(status("X4 build", scan_state, scan_text));
        int gpu_state = 3;
        const auto gpu_text = read_gpu_summary(gpu_state);
        items.push_back(status("GPU", gpu_state, gpu_text));
        items.push_back(status("Steam launch option", c.option.state == 1 ? 0 : 2,
            c.option.state == 1 ? (c.option.value == make_launch_option() ? "set" : "set: "+c.option.value) : c.option.state == 2 ? "points to another x4vr-run: copy it again (Setup)"
            : c.option.state == 3 ? "set to something else: "+c.option.value : c.option.accounts ? "not set: copy it (Setup)" : "start X4 once from Steam first"));
        items.push_back(status("X4 settings for VR", c.settings_to_fix < 0 ? 3 : c.to_fix.empty() ? 0 : 1,
            c.settings_to_fix == -1 ? "X4's config.xml not found (start X4 once)" : c.settings_to_fix == -2 ? "made at the first VR launch"
            : c.to_fix.empty() ? "ready (your 2D settings are kept apart)"
            : c.settings_to_fix == 0 ? std::to_string(c.to_fix.size())+" to look at (list below)"
            : std::to_string(c.settings_to_fix)+" set at the next VR launch"+
              (int(c.to_fix.size()) > c.settings_to_fix ? ", "+std::to_string(int(c.to_fix.size())-c.settings_to_fix)+" to look at" : std::string())+" (list below)"));
        if (c.hud_off_in_x4) items.push_back(status("HUD extension", 1, "turned off in X4's Extensions menu: turn on \"X4 VR HUD distance\""));
        if (!c.x4) items.push_back(status("X4", 3, std::string("not running")+(c.vr_settings_in_place ? " (VR settings still in place: restored at the next start)" : "")));
        else if (!c.vr_settings_in_place) items.push_back(status("X4", 3, "running in 2D"));
        else if (const auto stats = headset_stats())
            items.push_back(status("X4 in VR", stats->late || stats->repeated > 2 ? 1 : 0, std::to_string(stats->fps)+" fps per eye pair, "
                                   +std::to_string(stats->late)+" late, "+std::to_string(stats->repeated)+" repeated (last 2 s)"));
        else items.push_back(status("X4 in VR", 1, "running, no frames reaching the headset yet"));

        items.push_back(section("Play"));
        if (!c.x4) items.push_back(action("launch", "Launch X4 in VR",
            "Starts SteamVR if needed, then X4 through Steam with the mod. Steam's own Play button starts the normal 2D game."));
        else if (c.vr_settings_in_place) {
            items.push_back(action("recenter", "Recentre the view  (Ctrl+F12)", "Look straight ahead first. Same as Ctrl+F12 in X4, or SteamVR's recentre.", live));
            items.push_back(action("flat", "Flat screen on / automatic  (Ctrl+F11)", "Same as Ctrl+F11 in X4: the game on a flat screen in front of you.", live));
        } else items.push_back(info("X4 is running in 2D: quit it to launch in VR."));

        if (!board.empty()) {
            items.push_back(section("Notices"));
            for (const auto& [kind, text] : board) items.push_back(notice(kind, text));
        }

        items.push_back(section("Settings"));
        items.push_back(info("live: applies at once, also while X4 runs.   next launch: applied when X4 next starts in VR."));
        {
            const auto list = profiles();
            const auto name = current_profile();
            std::vector<std::string> names;
            int at = -1;
            for (size_t i = 0; i < list.size(); ++i) {
                const bool current = list[i].name == name;
                if (current) at = int(i);
                names.push_back(list[i].name+(list[i].built_in ? " (built in)" : "")+(current && !profile_matches(list[i]) ? " · changed" : ""));
            }
            if (at < 0) { names.insert(names.begin(), "(none)"); at = 0; }
            items.push_back(choice("profile", "Profile", names, at,
                "←→ loads another set of the settings below. Built in: Steam Frame. Change settings, then \"Save as profile\" to keep them.", live));
            items.push_back(action("profile_save", "Save as profile", "Saves the settings below under a name of your choice ("+user_profiles().string()+").", {}));
            if (at < int(list.size()) && name == list[size_t(at)].name && !list[size_t(at)].built_in)
                items.push_back(action("profile_delete", "Delete profile \""+name+"\"", "Your settings stay as they are.", {}));
        }
        items.push_back(status("VR runtime", 3, "SteamVR (OpenVR)"));
        items.push_back(toggle("stereo", "3D (stereo)", num("stereo", "1") != 0,
            "Each eye gets its own image. Off: both eyes see the same flat image (for comparing, or if 3D feels wrong).", live));
        items.push_back(toggle("shared_pose", "Shared pose per eye pair", num("shared_pose", "1") != 0,
            "Both eyes of a pair are submitted with one head pose. Keep on for the Steam Frame: without it the right eye ghosts.", live));
        items.push_back(number("ipd_scale", "World scale", num("ipd_scale", "1"), 0.05, 0.3, 3, 2, "",
            "The distance between your eyes in the game. Below 1 the world looks bigger, above 1 smaller. 1 is real size.", live));
        items.push_back(number("predict", "Head prediction", num("predict", "0.035")*1000, 1, 0, 100, 0, "ms",
            "How far ahead the head pose is predicted for X4. Higher: less lag when turning your head, but more wobble.", live));
        items.push_back(toggle("async_submit", "Stutter protection", num("async_submit", "1") != 0,
            "Sends an image to SteamVR every headset frame; a late game frame repeats the last image instead of a flash. Keep it on: "
            "off is for troubleshooting only, and loses the flat screen (menus) and the Steam Frame's shared pose.", next));
        items.push_back(toggle("cursor", "Mouse cursor in VR", num("cursor", "1") != 0, "Draws the mouse cursor in the headset.", live));
        {
            const int theater = int(num("theater", "1"));
            items.push_back(choice("theater", "Flat screen", {"automatic", "always", "never"}, theater == 2 ? 1 : theater == 0 ? 2 : 0,
                "Automatic: menus, the map and views without ship controls go to a flat screen in front of you (Ctrl+F11 switches).", live));
        }
        items.push_back(number("theater_distance", "Flat screen distance", num("theater_distance", "2"), 0.25, 0.5, 10, 2, "m",
            "How far in front of you the flat screen stands.", live));
        items.push_back(number("theater_width", "Flat screen width", num("theater_width", "2.2"), 0.1, 0.5, 10, 2, "m",
            "The flat screen's width; its height follows X4's image.", live));
        items.push_back(toggle("hud_on", "HUD Scaled", hud_factor > 0,
            "X4's cockpit HUD sits a hand's width from your face. On: an extension moves it back at the same apparent size, in VR "
            "only. X4 counts as modified then (saves flagged); turn Protected UI Mode off in X4's Extension Settings.", next));
        if (hud_factor > 0)
            items.push_back(number("hud_factor", "HUD Scale Ratio", hud_factor, 0.1, 1, 6, 1, "",
                "How many times farther the HUD is, from 1.0 to 6.0 (2.5 is a good start). Enter to type a value.", next));
        {
            const int w = int(num("x4_width", "0")), h = int(num("x4_height", "0"));
            std::vector<std::string> names{"automatic"};
            int at = 0;
            for (const auto& [mw, mh] : modes) { names.push_back(std::to_string(mw)+"x"+std::to_string(mh)); if (mw == w && mh == h) at = int(names.size())-1; }
            if (w <= 0 || h <= 0) custom_resolution = false;         // automatic, e.g. after loading a profile
            else if (at == 0) custom_resolution = true;               // a size not in the list
            names.push_back(custom_resolution ? "Custom: "+std::to_string(w)+"x"+std::to_string(h) : "Custom");
            if (custom_resolution) at = int(names.size())-1;
            items.push_back(choice("resolution", "X4 resolution in VR", names, at,
                "Automatic: the smallest 16:9 size that covers what SteamVR renders (no wasted pixels). Lower is faster, higher "
                "sharper. Custom: any width and height; the image keeps X4's vertical view and any aspect works.", next));
            if (custom_resolution) {
                items.push_back(number("x4_width", "Resolution Width", w, 16, 640, 16384, 0, "px",
                    "X4's image width in VR. Enter to type it.", next));
                items.push_back(number("x4_height", "Resolution Height", h, 16, 360, 16384, 0, "px",
                    "X4's image height in VR. Enter to type it.", next));
            }
        }

        items.push_back(section("X4 settings for VR"));
        items.push_back(action("checklist", "In-game settings checklist",
            "Every X4 setting VR needs, by where it is in X4's settings, with what X4 has now. For troubleshooting."));
        if (!c.to_fix.empty()) {
            items.push_back(info("X4 VR uses its own copy of X4's settings: use the checklist to check your settings, required settings will automatically be set for you at launch."));
            for (const auto& fix : c.to_fix) {
                using x4vr::linux_port::Kind;
                const auto now = fix.current.empty() ? std::string() : ", now: "+fix.current;
                items.push_back(status(fix.label, fix.kind == Kind::required ? 2 : 1,
                    fix.kind == Kind::required ? "required"+now : fix.kind == Kind::optional ? "optional"+now
                    : "warning"+now+": "+std::string(x4vr::linux_port::frame_limit_reason)));
                if (fix.kind != Kind::required) items.back().mark = fix.kind == Kind::optional ? "·" : "!";
            }
        }

        items.push_back(section("Setup"));
        items.push_back(action("option_copy", "Copy the Steam launch option",
            "Paste it in Steam: X4 > Properties > General > Launch options. Steam's Play still starts the normal game."));
        items.push_back(action("tiling", "Tiling window manager rules",
            "Hyprland, Sway, i3: a rule that floats the VR window (class X4VR) so X4 keeps the VR resolution. 2D isn't affected."));
        if (!std::filesystem::exists(desktop_file())) items.push_back(action("desktop", "Add to the app launcher", "\"X4 VR\" in your desktop's app menu, rofi or wofi: "+desktop_file().string()));
        items.push_back(action("report", "Make a bug report", "Packs logs (the mod's and SteamVR's), settings, your setup and a summary into ~/x4vr-report-<time>.tar.gz, to attach to a GitHub issue. Make it while X4 is stuck, if it is: then it also records where."));
        items.push_back(action("uninstall", "Uninstall", "Removes the desktop entry, HUD extension, the mod's copies of X4's settings, and its settings."));
        items.push_back(action("quit", "Quit"));

        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back) continue; // Esc doesn't close the menu: only Quit does
        if (e.id == "quit") return 0;
        if (e.kind == Event::Changed) {
            const auto& item = e.item;
            std::string key = e.id, value;
            if (e.id == "profile") {
                const auto list = profiles();
                const size_t index = size_t(item.choice)-(current_profile().empty() || std::none_of(list.begin(), list.end(), [](const auto& p) { return p.name == current_profile(); }) ? 1 : 0);
                if (index < list.size()) menu.messages = {load_profile(list[index]) ? "Profile \""+list[index].name+"\" loaded." : "Can't write "+path.string()};
                continue;
            }
            if (e.id == "resolution") {
                int w = 0, h = 0;
                custom_resolution = item.choice == int(item.choices.size())-1;
                if (custom_resolution) { // start from the size in use: the choice before, else SteamVR's last
                    w = int(num("x4_width", "0")); h = int(num("x4_height", "0"));
                    if (w <= 0 || h <= 0) {
                        std::ifstream saved(get_state_dir()/"x4_resolution.txt");
                        if (!(saved >> w) || saved.get() != 'x' || !(saved >> h) || w <= 0 || h <= 0) { w = 2560; h = 1440; }
                    }
                } else if (item.choice > 0) std::sscanf(item.choices[size_t(item.choice)].c_str(), "%dx%d", &w, &h);
                const bool ok = x4vr::linux_port::write_setting(path, "x4_width", std::to_string(w)) && x4vr::linux_port::write_setting(path, "x4_height", std::to_string(h));
                if (!ok) menu.messages = {"Can't write "+path.string()};
                continue;
            }
            if (e.id == "hud_on") { key = "hud_factor"; value = item.on ? format(c.hud > 0 ? c.hud : 2.5, 1) : "0"; }
            else if (item.kind == Item::Toggle) value = item.on ? "1" : "0";
            else if (e.id == "theater") value = item.choice == 0 ? "1" : item.choice == 1 ? "2" : "0";
            else if (e.id == "predict") value = format(item.number/1000, 3);
            else value = format(item.number, item.decimals);
            if (!x4vr::linux_port::write_setting(path, key, value)) menu.messages = {"Can't write "+path.string()};
            continue;
        }
        if (e.kind != Event::Activate) continue;
        if (e.id == "launch") {
            std::vector<Item> progress_items{section("Launch X4 in VR"), info("Esc: stop waiting")};
            Menu progress(terminal);
            progress.title = menu.title;
            progress.keys = "Esc stop waiting";
            progress.timeout_ms = 0;
            std::vector<std::string> lines;
            const auto show = [&](const std::string& line) { lines.push_back(line); progress.messages = lines; Event ignored; progress.step(progress_items, ignored); };
            const auto stop = [&] { return terminal.key(0) == Escape; };
            play(show, stop); // x4vr-run switches X4 to its VR settings, fixes them and applies the HUD distance
            menu.messages = lines;
        } else if (e.id == "recenter" || e.id == "flat") {
            const int now = x4vr::linux_port::control_settings(path, e.id);
            menu.messages = {now < 0 ? "Can't update "+path.string() : e.id == "recenter" ? "Recentred." : now == 2 ? "Flat screen on." : "Flat screen automatic."};
        } else if (e.id == "profile_save") {
            auto name = current_profile();
            for (const auto& p : profiles()) if (p.built_in && p.name == name) name.clear();
            if (const auto typed = menu.prompt(items, "Profile name", name)) menu.messages = {save_profile(*typed)};
        } else if (e.id == "profile_delete") {
            std::error_code error;
            const auto name = current_profile();
            std::filesystem::remove(user_profiles()/(name+".txt"), error);
            std::filesystem::remove(get_state_dir()/"profile", error);
            menu.messages = {"Profile \""+name+"\" deleted; your settings stay as they are."};
        } else if (e.id == "option_copy") {
            const auto how = copy_to_clipboard(make_launch_option());
            menu.messages = {"Copied (" + how + "): " + make_launch_option(), launch_option_steps()};
        } else if (e.id == "desktop") menu.messages = {install_desktop()};
        else if (e.id == "report") {
            const Report report = make_report();
            if (report.file.empty()) menu.messages = {"Couldn't write the report: "+report.error};
            else menu.messages = {"Report: "+report.file, "Attach it to a GitHub issue: "+std::string(issues_url)};
        } else if (e.id == "uninstall") uninstall_screen(terminal);
        else if (e.id == "checklist") checklist_screen(terminal);
        else if (e.id == "tiling") tiling_screen(terminal);
    }
}
}  // namespace
}  // namespace x4vr::linux_launcher
using namespace x4vr::linux_launcher;   // for patterns() and main(), until x4vr.cpp

// The X4 scan the mod runs at startup (code_scan.hpp), on the executable file.
int patterns(const std::vector<std::string_view>& args) {
    if (args.size() > 1) { usage(); return 2; }
    const std::filesystem::path path = args.empty() ? find_x4_dir()/"X4" : std::filesystem::path(args[0]);
    if (path.empty() || !std::filesystem::exists(path)) { std::cerr << "X4 not found; give its path\n"; return 1; }
    const auto sites = x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load(path.string()));
    std::cout << path.string() << ":\n";
    for (const auto& note : sites.notes) std::cout << "  " << note << '\n';
    const bool all = sites.backward_clamp && sites.onfoot_zeroing && sites.camera_offset && sites.frame_half_global && sites.opentrack_vtable;
    std::cout << (all ? "All found: the mod supports this X4.\n" : "Some not found: those features stay off in this X4.\n");
    return all ? 0 : 1;
}

int main(int argc, char** argv) {
    { // this executable as started, unresolved, so a symlinked install path stays as given
        std::error_code error;
        const std::string first = argc > 0 ? argv[0] : "";
        if (first.find('/') != std::string::npos) program = std::filesystem::absolute(first, error).lexically_normal(); // ./x -> /dir/x
        else if (const char* path = std::getenv("PATH")) {
            std::istringstream dirs(path);
            for (std::string dir; std::getline(dirs, dir, ':');)
                if (!dir.empty() && access((std::filesystem::path(dir)/first).c_str(), X_OK) == 0) { program = std::filesystem::path(dir)/first; break; }
        }
        if (program.empty()) program = std::filesystem::read_symlink("/proc/self/exe", error);
    }
    if (argc < 2) return menu();
    const std::string_view command = argv[1];
    const std::vector<std::string_view> args(argv+2, argv+argc);
    try {
        if (command == "menu") return menu();
        if (command == "uninstall") return cmd_uninstall(args);
        if (command == "settings-mode") return cmd_settings_mode(args);
        if (command == "report" && args.empty()) {
            const Report report = make_report();
            if (report.file.empty()) { std::cerr << "Couldn't write the report: " << report.error << '\n'; return 1; }
            std::cout << "Report: " << report.file << "\nAttach it to a GitHub issue: " << issues_url << '\n';
            return 0;
        }
        if (command == "ctl") return ctl(args);
        if (command == "hud" && args.size() == 1 && args[0] == "--refresh") return refresh_hud();
        if (command == "hud" && args.size() == 1 && args[0] == "--park") return park_hud();
        if (command == "patterns") return patterns(args);
        if (command == "vr-vulkan" && args.empty()) return vr_vulkan();
        if (command == "fix-settings" && args.size() <= 1 && (args.empty() || args[0] == "--auto"))
            return fix_x4_settings(!args.empty());
        if (command == "help" || command == "--help" || command == "-h") { usage(); return 0; }
        if (command == "version" || command == "--version") { std::cout << make_build_text(); return 0; }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "x4vr " << command << ": " << error.what() << '\n';
        return 1;
    }
}
