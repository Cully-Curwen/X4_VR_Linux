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
#include "ui/terminal_ui.hpp"
#include "common.hpp"
#include "x4.hpp"
#include "hud.hpp"
#include "install.hpp"
#include "launch.hpp"
#include "report.hpp"
#include "status.hpp"
#include "ui/menu.hpp"
#include "ui/profiles.hpp"
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

// `x4vr uninstall [name]...`: removes only what is named (no "all": each removal is chosen); alone,
// lists the names and what each removes, removing nothing. As the menu's Uninstall screen.
int cmd_uninstall(const std::vector<std::string_view>& args) {
    std::vector<Removal> chosen = make_removals();
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
    const std::filesystem::path source = find_source_dir();
    std::cout << "Then, by hand: clear X4's launch option in Steam (X4 > Properties > General > Launch options)"
              << (source.empty() ? std::string(".") : "; delete the GitHub clone directory: "+source.string()) << '\n';
    return 0;
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
    if (argc < 2) return show_menu();
    const std::string_view command = argv[1];
    const std::vector<std::string_view> args(argv+2, argv+argc);
    try {
        if (command == "menu") return show_menu();
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
