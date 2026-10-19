#include "status.hpp"
#include "common.hpp"
#include "gpu_status.hpp"
#include "hud.hpp"
#include "launch.hpp"
#include "steam_config.hpp"
#include "x4.hpp"
#include "x4_settings.hpp"
#include "../launcher/hud_mod.hpp"
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>

namespace x4vr::linux_launcher {

Status read_status() {
    Status status;
    status.steam = x4vr::steam::steam_running();
    status.steamvr = is_steamvr_running();
    status.x4 = is_x4_running();
    status.vr_settings_in_place = std::filesystem::exists(get_settings_marker());
    status.option = read_launch_option();

    // The HUD extension: its factor, and whether X4's Extensions menu turned it off during VR
    // (outside VR it is off on purpose).
    if (const std::filesystem::path game = find_x4_dir(); !game.empty()) {
        const std::map<std::string, std::string> installed = find_installed_hud(game);
        status.hud = installed.count("scale") ? std::atof(installed.at("scale").c_str()) : 0;
    }
    const std::filesystem::path config = x4_config();
    if (config.empty()) return status;
    bool disabled = false;
    x4vr::launcher::enable_extension(read_text(config.parent_path()/"content.xml"), "x4vr_hud", disabled);
    status.hud_off_in_x4 = status.hud > 0 && disabled && status.vr_settings_in_place;

    // X4's VR settings: config.xml during a VR session, else the VR copy, made at the first VR launch.
    const std::filesystem::path vr = config.string()+".x4vr-vr";
    const std::string xml = read_text(status.vr_settings_in_place ? config : vr);
    status.settings_to_fix = status.vr_settings_in_place || std::filesystem::exists(vr) ? 0 : -2;
    if (status.settings_to_fix != 0) return status;
    for (const x4vr::launcher::Check& check : x4_checks(xml)) {
        if (check.ok || (check.required && !x4vr::linux_port::to_write(check, xml))) continue;
        if (check.required) ++status.settings_to_fix;
        status.to_fix.push_back({x4vr::linux_port::kind(check), check.label, check.current});
    }
    return status;
}

std::string read_gpu_summary(int& state) {
    for (const char* name : {"x4vr.log", "x4vr.previous.log"})
        if (const std::optional<x4vr::linux_port::GpuStatus> gpu = x4vr::linux_port::gpu_from_log(read_text(get_state_dir()/name))) {
            state = gpu->state;
            return gpu->text;
        }
    state = 3;
    return "known after the first VR launch";
}

std::string make_scan_summary(const x4vr::linux_port::code::X4Sites& sites, int& state) {
    const bool all = sites.backward_clamp && sites.onfoot_zeroing && sites.camera_offset && sites.frame_half_global && sites.opentrack_vtable;
    const bool core = sites.opentrack_vtable && sites.frame_half_global;
    if (all) { state = 0; return "all VR features supported"; }
    if (core) { state = 1; return "some features off in this X4 (see Bug report)"; }
    state = 2;
    return "this X4 build isn't supported";
}

} // namespace x4vr::linux_launcher
