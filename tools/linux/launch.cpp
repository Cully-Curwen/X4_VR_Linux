// Starting X4 in VR. The Steam launch option `x4vr-run %command%` stays set; x4vr-run starts VR only
// when a launch request from the menu is fresh (linux/x4vr-run.in), so Steam's Play button starts
// the normal game. `steam -applaunch` asks the running Steam client to start X4 with Steam's usual
// chain (reaper, the Steam Linux Runtime); the game inherits Steam's environment, not ours.

#include "launch.hpp"
#include "common.hpp"
#include "steam_config.hpp"
#include "x4.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

namespace x4vr::linux_launcher {

// This install's x4vr-run, next to this executable: what X4's launch option has to run.
static std::filesystem::path get_run_script() { return program.parent_path()/"x4vr-run"; }

std::string make_launch_option() { return quote_path(get_run_script())+" %command%"; }

x4vr::linux_port::LaunchOption read_launch_option() {
    std::vector<std::string> values;
    for (const std::filesystem::path& file : x4vr::steam::local_configs())
        if (const std::optional<std::string> value = x4vr::steam::launch_options(x4vr::steam::read_file(file))) values.push_back(*value);
    return x4vr::linux_port::judge_launch_options(values, get_run_script().string());
}

bool is_steamvr_running() { return x4vr::steam::process_named("vrserver"); }

bool play(const std::function<void(const std::string&)>& progress, const std::function<bool()>& cancelled) {
    // Only with this install's launch option: without it, Steam starts X4 without the mod.
    // X4VR_ANY_RUN=1 accepts another install's x4vr-run (state 2, launch_option.hpp).
    if (is_x4_running()) { progress("X4 is already running."); return false; }
    const x4vr::linux_port::LaunchOption option = read_launch_option();
    const bool other_run_accepted = option.state == 2 && std::getenv("X4VR_ANY_RUN");
    if (option.state != 1 && !other_run_accepted) {
        if (option.accounts == 0) progress("Start X4 once from Steam first, then set its launch option.");
        else if (option.state == 2) progress("X4's launch option points to another x4vr-run: copy the new one (menu: Copy the launch option).");
        else progress("Set X4's launch option first (menu: Copy the launch option).");
        return false;
    }

    // SteamVR first, started through Steam if it isn't running, waiting up to 90 s for it.
    if (!is_steamvr_running()) {
        progress("Starting SteamVR... (put the headset on and connect it)");
        run({"steam", "steam://rungameid/250820"}, {.detach = true});
        for (int i = 0; i < 180 && !is_steamvr_running(); ++i) {
            if (cancelled()) { progress("Cancelled."); return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!is_steamvr_running()) { progress("SteamVR didn't start within 90 s; start it from Steam and try again."); return false; }
        progress("SteamVR is running.");
    }

    // The request x4vr-run looks for, then X4 through Steam, waiting up to 2 minutes for it.
    std::error_code error;
    std::filesystem::create_directories(get_state_dir(), error);
    if (!write_text(get_state_dir()/"launch.request", std::to_string(std::time(nullptr))+"\n")) { progress("Can't write the launch request."); return false; }
    progress("Starting X4 through Steam... If the headset isn't connected yet, X4 waits up to 2 minutes for it: put it on.");
    run({"steam", "-applaunch", std::string(x4vr::steam::x4_app)}, {.detach = true});
    for (int i = 0; i < 240 && !is_x4_running(); ++i) {
        if (cancelled()) { progress("Stopped waiting; X4 may still start."); return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    progress(is_x4_running() ? "X4 is running in VR. Recentre: Ctrl+F12 or SteamVR's menu." : "X4 didn't start within 2 minutes; check Steam.");
    return is_x4_running();
}

} // namespace x4vr::linux_launcher
