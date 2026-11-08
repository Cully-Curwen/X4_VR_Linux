// X4's settings (its config.xml) for VR: the checks, fix-settings, and the resolution X4 should
// run at in VR.

#include "x4_settings.hpp"
#include "common.hpp"
#include "hud.hpp"
#include "settings_control.hpp"
#include "x4.hpp"
#include "x4_settings_rules.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <openvr.h>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace x4vr::linux_launcher {

// What the mod computes at startup (src/linux/observe_layer.cpp, presenter_initialize): X4's image spans
// 2*tan_x by 2*tan_y (tan_y = game_tan_y, X4's FOV), at the pixels per tangent SteamVR recommends.
// Asked as a background app: it doesn't start SteamVR or show up as a running game.
static bool query_steamvr_resolution(const std::string& xml, int& w, int& h) {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::IVRSystem* system = vr::VR_Init(&error, vr::VRApplication_Background);
    if (!system || error != vr::VRInitError_None) return false;

    uint32_t rec_w = 0, rec_h = 0;
    system->GetRecommendedRenderTargetSize(&rec_w, &rec_h);
    double want_x = 0, want_y = 0;
    for (int e = 0; e < 2; ++e) {
        float l, r, t, b;
        system->GetProjectionRaw(vr::EVREye(e), &l, &r, &t, &b);
        if (r > l) want_x = std::max(want_x, rec_w/double(r-l));
        if (b != t) want_y = std::max(want_y, rec_h/std::fabs(double(b-t)));
    }
    vr::VR_Shutdown();

    const double tan_y = std::atof(x4vr::linux_port::read_setting(get_settings_file(), "game_tan_y", "0.8675").c_str());

    std::string rw, rh;
    const double aspect = x4vr::launcher::xml_value(xml, "res_width", rw) && x4vr::launcher::xml_value(xml, "res_height", rh) &&
                          std::atoi(rh.c_str()) > 0 ? std::atof(rw.c_str())/std::atof(rh.c_str()) : 16.0/9;
    w = int(std::lround(2*tan_y*aspect*want_x));
    h = int(std::lround(2*tan_y*want_y));
    return w > 0 && h > 0;
}

// X4's resolution: the smallest common 16:9 mode at least as large as what SteamVR uses (saved by
// SteamVR directly when it runs, else saved by the mod in x4_resolution.txt while X4 ran), so X4
// doesn't render pixels SteamVR throws away. Chosen in the menu: stereo.txt x4_width/x4_height.
// X4VR_RESOLUTION=WxH sets it, =0 leaves it alone. SteamVR is asked only with `query` ask.
static Resolution compute_vr_resolution(const std::string& xml, SteamVrQuery query) {
    int w = 0, h = 0;
    if (const char* set = std::getenv("X4VR_RESOLUTION"); set && *set) {
        if (std::sscanf(set, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) return {w, h};
        return {0, 0};
    }

    // Chosen in the menu (stereo.txt x4_width / x4_height, as the Windows launcher); 0: automatic.
    w = std::atoi(x4vr::linux_port::read_setting(get_settings_file(), "x4_width", "0").c_str());
    h = std::atoi(x4vr::linux_port::read_setting(get_settings_file(), "x4_height", "0").c_str());
    if (w > 0 && h > 0) return {w, h};

    if (query == SteamVrQuery::ask && query_steamvr_resolution(xml, w, h)) {
        std::ofstream(get_state_dir()/"x4_resolution.txt", std::ios::trunc) << w << 'x' << h << '\n';
    } else { // not asked, or SteamVR not running: the size saved last time
        const std::optional<Resolution> last = read_last_vr_resolution();
        if (!last) return {0, 0};
        w = last->width;
        h = last->height;
    }

    for (const Resolution& resolution : vr_resolutions)
        if (resolution.width >= w && resolution.height >= h) return resolution;
    return vr_resolutions[std::size(vr_resolutions)-1];
}

std::optional<Resolution> read_last_vr_resolution() {
    std::ifstream in(get_state_dir()/"x4_resolution.txt");
    Resolution resolution{};
    if (!(in >> resolution.width) || in.get() != 'x' || !(in >> resolution.height)) return std::nullopt;
    if (resolution.width <= 0 || resolution.height <= 0) return std::nullopt;
    return resolution;
}

std::optional<std::string> read_x4_vr_settings() {
    const std::filesystem::path config = x4_config();
    if (config.empty()) return std::nullopt;
    if (std::filesystem::exists(get_settings_marker())) return read_text(config);
    const std::filesystem::path vr_copy = config.string()+".x4vr-vr";
    if (!std::filesystem::exists(vr_copy)) return std::nullopt;
    return read_text(vr_copy);
}

std::vector<x4vr::launcher::Check> x4_checks(const std::string& xml, SteamVrQuery query) {
    const Resolution resolution = compute_vr_resolution(xml, query);
    return x4vr::linux_port::linux_checks(xml, resolution.width, resolution.height, is_hud_scaled()); // x4_settings_rules.hpp
}

int fix_x4_settings(bool quiet) {
    const std::filesystem::path path = x4_config();
    if (path.empty()) { std::cerr << "X4's config.xml not found under ~/.config/EgoSoft/X4 (start X4 once).\n"; return 1; }
    const std::string xml = read_text(path);
    const std::vector<x4vr::launcher::Check> checks = x4_checks(xml, SteamVrQuery::ask);

    // Required settings written with their VR value, optional ones only shown (x4_settings_rules.hpp).
    std::vector<x4vr::launcher::Check> fixable;
    for (const x4vr::launcher::Check& c : checks) {
        const bool write = x4vr::linux_port::to_write(c, xml);
        if (write) fixable.push_back(c);

        // The row: its status, and a note after the value when x4vr won't fix it.
        using x4vr::linux_port::SettingSeverity;
        const char* status = "ok";
        std::string note;
        if (!c.ok) {
            switch (x4vr::linux_port::get_setting_severity(c)) {
            case SettingSeverity::required:
                status = "FIX";
                if (!write) note = "  (change it in X4)";
                break;
            case SettingSeverity::optional:
                status = "opt";
                note = "  (optional: change it in X4 if you like)";
                break;
            case SettingSeverity::warning:
                status = "warn";
                note = "  (" + std::string(x4vr::linux_port::frame_limit_reason) + ")";
                break;
            }
        }
        if (!quiet) std::printf("%-4s %-36s %s%s\n", status, c.label.c_str(), c.current.c_str(), note.c_str());
    }
    if (fixable.empty()) { if (!quiet) std::cout << "Nothing to fix.\n"; return 0; }
    if (is_x4_running()) { std::cerr << "X4 is running: close it first (it rewrites config.xml when it exits).\n"; return 1; }

    // The player's settings from before x4vr changed anything: made once, never overwritten.
    const std::filesystem::path backup = path.string()+".x4vr-backup";
    if (!std::filesystem::exists(backup)) {
        std::error_code error;
        std::filesystem::copy_file(path, backup, error);
        if (error) { std::cerr << "Can't back up " << path.string() << " (" << error.message() << "): nothing changed.\n"; return 1; }
    }

    if (!write_text(path, x4vr::linux_port::write_settings(xml, fixable))) { std::cerr << "Can't write " << path.string() << '\n'; return 1; }

    for (const x4vr::launcher::Check& c : fixable)
        std::cout << "x4vr: X4 setting set for VR: " << c.label << " (was " << c.current << ")\n";
    std::cout << "x4vr: " << path.string() << " updated; original kept as " << backup.string() << '\n';
    return 0;
}

x4vr::linux_port::Switched switch_x4_settings(bool vr) {
    // During a VR session the marker names X4's config.xml; else it's X4's own (none: never started).
    const std::filesystem::path in_vr = x4vr::linux_port::config_in_vr(get_settings_marker());
    const std::filesystem::path config = in_vr.empty() ? x4_config() : in_vr;
    if (config.empty()) return {};

    // X4 rewrites config.xml when it exits: give it up to 10 seconds to finish, checking every 0.1 s.
    for (int i = 0; i < 100 && is_x4_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (is_x4_running()) return {false, false, "X4 is running: its settings are switched when it isn't."};

    const x4vr::linux_port::Switched done = x4vr::linux_port::switch_settings(get_settings_marker(), config, vr);
    if (done.ok && done.changed && vr) set_hud_extension_enabled(config, true);
    return done;
}

} // namespace x4vr::linux_launcher
