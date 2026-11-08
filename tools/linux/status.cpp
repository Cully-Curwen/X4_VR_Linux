#include "status.hpp"
#include "common.hpp"
#include "elf_classes.hpp"
#include "gpu_status.hpp"
#include "hud.hpp"
#include "launch.hpp"
#include "steam_config.hpp"
#include "x4.hpp"
#include "x4_settings.hpp"
#include "../launcher/hud_mod.hpp"
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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

    // X4's VR settings (x4_settings.hpp), made at the first VR launch.
    const std::optional<std::string> xml = read_x4_vr_settings();
    if (!xml) {
        status.settings_to_fix = -2;
        return status;
    }
    status.settings_to_fix = 0;
    for (const x4vr::launcher::Check& check : x4_checks(*xml, SteamVrQuery::skip)) {
        if (check.ok || (check.required && !x4vr::linux_port::to_write(check, *xml))) continue;
        if (check.required) ++status.settings_to_fix;
        status.to_fix.push_back({x4vr::linux_port::get_setting_severity(check), check.label, check.current});
    }
    return status;
}

std::optional<x4vr::launcher::Stats> read_headset_stats() {
    const std::filesystem::path path = get_state_dir()/"pair_stats.txt";
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
    if (error || std::filesystem::file_time_type::clock::now()-time > std::chrono::seconds(6)) return std::nullopt;

    std::ifstream in(path);
    std::string line, last;
    while (std::getline(in, line))
        if (!line.empty()) last = line;
    x4vr::launcher::Stats stats{};
    if (!x4vr::launcher::parse_stats_line(last, stats)) return std::nullopt;
    return stats;
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

X4Scan scan_x4(const std::filesystem::path& program) {
    X4Scan scan;
    if (!program.empty()) scan.program = program;
    else if (const std::filesystem::path game = find_x4_dir(); !game.empty()) scan.program = game/"X4";
    if (scan.program.empty() || !std::filesystem::exists(scan.program)) {
        scan.program.clear();
        return scan;
    }
    try {
        scan.sites = x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load(scan.program.string()));
        scan.result = X4Scan::Result::scanned;
    } catch (const std::exception& e) {
        scan.result = X4Scan::Result::unreadable;
        scan.error = e.what();
    }
    return scan;
}

std::string make_scan_summary(const X4Scan& scan, int& state) {
    switch (scan.result) {
    case X4Scan::Result::not_found:
        state = 3;
        return "X4 not found (set X4VR_GAME_DIR)";
    case X4Scan::Result::unreadable:
        state = 2;
        return "X4 found, but its program couldn't be read: "+scan.error;
    case X4Scan::Result::scanned:
        break;
    }
    const x4vr::linux_port::code::X4Sites& sites = scan.sites;
    const bool all = sites.backward_clamp && sites.onfoot_zeroing && sites.camera_offset && sites.frame_half_global && sites.opentrack_vtable;
    const bool core = sites.opentrack_vtable && sites.frame_half_global;
    if (all) { state = 0; return "all VR features supported"; }
    if (core) { state = 1; return "some features off in this X4 (see Bug report)"; }
    state = 2;
    return "this X4 build isn't supported";
}

} // namespace x4vr::linux_launcher
