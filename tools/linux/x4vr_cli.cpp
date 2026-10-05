// x4vr: the Linux port's tool. Without arguments, a full-screen menu (checks, VR settings, Launch
// in VR, HUD distance, bug report, uninstall); its actions and the Phase 0 measurements
// (docs/LINUX_PORT_PLAN.md) are also subcommands (x4vr help).
#include "elf_classes.hpp"
#include "code_scan.hpp"
#include "../launcher/hud_mod.hpp"
#include "../launcher/launcher_settings.hpp"
#include "md5.hpp"
#include "settings_control.hpp"
#include "opentrack.hpp"
#include "steam_config.hpp"
#include "terminal_ui.hpp"
#include <x4vr/session.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
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
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr double pi = 3.14159265358979323846;

void usage() {
    std::cout <<
        "Usage: x4vr [command] [options]\n"
        "\n"
        "  (no command) or menu\n"
        "      The menu: checks, VR settings, Launch X4 in VR, HUD distance, bug report, uninstall.\n"
        "\n"
        "  launch\n"
        "      Launch X4 in VR: starts SteamVR if needed, then X4 through Steam (steam -applaunch) with\n"
        "      the mod. Steam's own Play button starts the normal game.\n"
        "\n"
        "  launch-option status | copy\n"
        "      X4's Steam launch option (x4vr-run %command%), needed for Launch: shows whether it is\n"
        "      set, or copies it to paste in Steam (X4 > Properties > General > Launch options).\n"
        "\n"
        "  settings-mode vr | 2d | status\n"
        "      Keeps X4's settings for 2D and VR apart (config.xml.x4vr-2d / -vr): x4vr-run switches to\n"
        "      VR before a VR launch and back to 2D when X4 exits.\n"
        "\n"
        "  install-desktop\n"
        "      Adds \"X4 VR\" (this menu, in a terminal) to the app launcher (rofi, desktop menus).\n"
        "\n"
        "  report\n"
        "      Packs logs, settings and a summary into ~/x4vr-report-<time>.tar.gz for a bug report.\n"
        "\n"
        "  uninstall\n"
        "      Removes what the mod set up (desktop entry, HUD extension, its settings and copies).\n"
        "\n"
        "  vr-check [--seconds N]\n"
        "      Connects to SteamVR (OpenVR), prints the headset, render size and eye positions,\n"
        "      then the head pose 4 times a second for N seconds (default 10). Start SteamVR first.\n"
        "\n"
        "  udp-send [--host 127.0.0.1] [--port 4242] [--rate 90]\n"
        "      Sends OpenTrack UDP head poses (as X4 expects with OpenTrack Support on) and reads\n"
        "      commands from the terminal. Type 'help' once it runs.\n"
        "\n"
        "  check\n"
        "      Checks X4's settings (config.xml) for VR: FOV, anti-aliasing, upscaling, frame\n"
        "      generation, VSync, frame rate limit, OpenTrack support, and the resolution SteamVR\n"
        "      uses, in windowed mode (X4VR_RESOLUTION=WxH or 0 overrides).\n"
        "\n"
        "  fix-settings [--auto]\n"
        "      Sets what check reports, keeping everything else (backup: config.xml.x4vr-backup,\n"
        "      made once). Refuses while X4 runs: X4 rewrites config.xml when it exits. --auto: quiet\n"
        "      unless something changed (x4vr-run uses it before every start).\n"
        "\n"
        "  hud <factor> | remove | status | --refresh\n"
        "      Moves X4's cockpit HUD <factor> times further away at the same apparent size (1 to 6;\n"
        "      2.5 is a good start): writes the extension extensions/x4vr_hud, built from your own game\n"
        "      files, as the Windows launcher does. X4 must be closed. --refresh rebuilds it after a\n"
        "      game update (x4vr-run does that before every start). Game folder: $X4VR_GAME_DIR, else\n"
        "      Steam's default library. X4 then counts as modified (no online features; saves made\n"
        "      with it stay flagged).\n"
        "\n"
        "  patterns [path to the X4 executable]\n"
        "      Finds the X4 code the mod patches and hooks by its bytes, as the mod does at startup,\n"
        "      and prints where (default: X4 in the game folder). A site it doesn't find stays\n"
        "      unpatched in the game; the mod logs the same list (\"X4VR scan\").\n"
        "\n"
        "  game-grep <text-regex> [path-regex]\n"
        "      Searches the game's catalog files (the base game's copy of each file) whose path matches\n"
        "      [path-regex] (default: UI scripts and assets) and prints path:line: text for each match.\n"
        "\n"
        "  ctl recenter | flat\n"
        "      While X4 runs with x4vr-run: recentre the view (and the virtual screen), or switch\n"
        "      the flat virtual screen on/off. Edits stereo.txt in $X4VR_DIR (default\n"
        "      ~/.local/state/x4vr). Bind them to keys in your desktop (Ctrl+F12 / Ctrl+F11 on Windows).\n"
        "\n"
        "  elf-classes <executable> <filter>\n"
        "      Lists C++ classes whose type name contains <filter> (e.g. Track), with their base\n"
        "      classes and vtables, from the RTTI of a non-PIE executable such as Linux X4.\n";
}

template<class T> bool parse_number(std::string_view text, T& value) {
    const auto end = text.data()+text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc() && ptr == end;
}

// ---- vr-check -------------------------------------------------------------------------------
void print_pose(const char* label, const x4vr::Matrix& m) {
    // OpenVR seated: +X right, +Y up, -Z forward; R = Ry(yaw) * Rx(pitch) * Rz(roll).
    const double yaw = std::atan2(m.m[0][2], m.m[2][2])*180/pi;
    const double pitch = std::asin(std::fmax(-1.0, std::fmin(1.0, -double(m.m[1][2]))))*180/pi;
    const double roll = std::atan2(m.m[1][0], m.m[1][1])*180/pi;
    std::printf("%s position_m=(%+.3f, %+.3f, %+.3f) yaw=%+7.2f pitch=%+7.2f roll=%+7.2f\n",
                label, m.m[0][3], m.m[1][3], m.m[2][3], yaw, pitch, roll);
}
int vr_check(const std::vector<std::string_view>& args) {
    int seconds = 10;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i+1 < args.size() && parse_number(args[i+1], seconds) && seconds >= 0) ++i;
        else { usage(); return 2; }
    }
    std::cout << "runtime_installed=" << vr::VR_IsRuntimeInstalled() << '\n';
    char path[4096]{};
    uint32_t needed = 0;
    if (vr::VR_GetRuntimePath(path, sizeof(path), &needed)) std::cout << "runtime_path=" << path << '\n';
    // Only informative: on Linux this quick check can report no headset while SteamVR has one
    // (streamed headsets, or SteamVR's client library failing to load here). Connecting tells why.
    std::cout << "headset_present=" << vr::VR_IsHmdPresent() << '\n' << std::flush;
    x4vr::Session session;
    try {
        session.initialize();
    } catch (const std::exception& error) {
        std::cerr << "Connecting to SteamVR failed: " << error.what() << "\n"
                     "Is SteamVR running with the headset? If it is, try again through steam-run (docs/LINUX_PHASE0.md, step 3).\n";
        return 4;
    }
    std::cout << "headset_model=" << session.headset_model() << '\n';
    std::cout << "required_instance_extensions=";
    for (const auto& ext : session.instance_extensions()) std::cout << ext << ' ';
    std::cout << '\n';
    x4vr::Frame frame;
    auto status = x4vr::FrameStatus::tracking_unavailable;
    for (auto deadline = std::chrono::steady_clock::now()+5s; std::chrono::steady_clock::now() < deadline;) {
        status = session.begin_frame(frame, 0.05f, 10000.f);
        if (status == x4vr::FrameStatus::ready || status == x4vr::FrameStatus::quit_requested) break;
        std::this_thread::sleep_for(20ms);
    }
    if (status != x4vr::FrameStatus::ready) {
        std::cerr << "frame_unavailable=" << int(status) << ' ' << session.last_error() << '\n';
        return 5;
    }
    std::cout << "per_eye_render_size=" << frame.width << 'x' << frame.height << '\n';
    for (int e = 0; e < 2; ++e) print_pose(e ? "right_eye" : "left_eye ", frame.eyes[e].seated_from_eye);
    std::cout << "Head pose for " << seconds << " s (seated space; move your head):\n" << std::flush;
    int good = 0, bad = 0;
    for (auto end = std::chrono::steady_clock::now()+std::chrono::seconds(seconds); std::chrono::steady_clock::now() < end;) {
        x4vr::Matrix head;
        status = session.sample_tracking(head);
        if (status == x4vr::FrameStatus::quit_requested) break;
        if (status == x4vr::FrameStatus::ready) { ++good; print_pose("head", head); }
        else { ++bad; std::puts("head tracking_unavailable"); }
        std::fflush(stdout);
        std::this_thread::sleep_for(250ms);
    }
    session.shutdown();
    std::cout << "samples_tracked=" << good << " samples_untracked=" << bad << '\n';
    return good > 0 || seconds == 0 ? 0 : 5;
}

// ---- udp-send -------------------------------------------------------------------------------
struct Sender {
    std::mutex mutex;
    x4vr::opentrack::Pose base;
    int alt_axis = -1; double alt_value = 0;               // alternates +value/-value per packet
    int sweep_axis = -1; double sweep_amplitude = 0, sweep_period = 4; // sine on one axis
    bool paused = false;
    std::atomic<uint64_t> sent{0}, failed{0};
};
std::string describe(Sender& s) {
    std::lock_guard lock(s.mutex);
    std::ostringstream out;
    out << "pose:";
    for (int i = 0; i < 6; ++i) out << ' ' << x4vr::opentrack::axis_names[i] << '=' << s.base.axis(i);
    if (s.alt_axis >= 0) out << " | alt " << x4vr::opentrack::axis_names[s.alt_axis] << " +-" << s.alt_value;
    if (s.sweep_axis >= 0) out << " | sweep " << x4vr::opentrack::axis_names[s.sweep_axis] << " +-" << s.sweep_amplitude << " every " << s.sweep_period << " s";
    out << (s.paused ? " | PAUSED" : "") << " | sent " << s.sent << " failed " << s.failed;
    return out.str();
}
void udp_help() {
    std::cout <<
        "Commands (OpenTrack units: x y z in cm, yaw pitch roll in degrees):\n"
        "  <axis> <value>                set one axis, e.g. 'yaw 30' or 'z -5'\n"
        "  zero                          all axes to 0, alternation and sweep off\n"
        "  alt <axis> <value> | alt off  alternate +value/-value every packet (smoothing test)\n"
        "  sweep <axis> <amplitude> <period_s> | sweep off\n"
        "  pause | resume                stop/start sending (what does X4 do without packets?)\n"
        "  show | help | quit\n";
}
int udp_send(const std::vector<std::string_view>& args) {
    std::string host = "127.0.0.1";
    int port = x4vr::opentrack::default_port;
    double rate = 90;
    for (size_t i = 0; i < args.size(); i += 2) {
        const auto key = args[i];
        const auto value = i+1 < args.size() ? args[i+1] : std::string_view();
        if (key == "--host" && !value.empty()) host = std::string(value);
        else if (key == "--port" && parse_number(value, port) && port > 0 && port < 65536) {}
        else if (key == "--rate" && parse_number(value, rate) && rate >= 1 && rate <= 1000) {}
        else { usage(); return 2; }
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) { std::cerr << "Not an IPv4 address: " << host << '\n'; return 2; }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    Sender sender;
    std::atomic_bool running{true};
    std::thread thread([&] {
        const auto start = std::chrono::steady_clock::now();
        const auto period = std::chrono::duration<double>(1.0/rate);
        auto next = start;
        for (uint64_t n = 0; running; ++n) {
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            x4vr::opentrack::Pose pose;
            bool paused;
            {
                std::lock_guard lock(sender.mutex);
                pose = sender.base;
                paused = sender.paused;
                if (sender.alt_axis >= 0) pose.axis(sender.alt_axis) += (n % 2 ? -1 : 1)*sender.alt_value;
                if (sender.sweep_axis >= 0) {
                    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                    pose.axis(sender.sweep_axis) += sender.sweep_amplitude*std::sin(2*pi*t/sender.sweep_period);
                }
            }
            if (!paused) {
                const auto packet = x4vr::opentrack::encode(pose);
                if (sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == ssize_t(packet.size())) ++sender.sent;
                else ++sender.failed;
            }
            std::this_thread::sleep_until(next);
        }
    });
    std::cout << "Sending to " << host << ':' << port << " at " << rate << " Hz.\n";
    udp_help();
    std::cout << describe(sender) << "\n> " << std::flush;
    for (std::string line; std::getline(std::cin, line); std::cout << "> " << std::flush) {
        std::istringstream in(line);
        std::string command;
        if (!(in >> command)) { std::cout << describe(sender) << '\n'; continue; }
        if (command == "quit" || command == "exit") break;
        if (command == "help") { udp_help(); continue; }
        bool ok = true;
        {
            std::lock_guard lock(sender.mutex);
            std::string axis_name; double value = 0, period = 0;
            if (command == "zero") { sender.base = {}; sender.alt_axis = sender.sweep_axis = -1; }
            else if (command == "pause") sender.paused = true;
            else if (command == "resume") sender.paused = false;
            else if (command == "show") {}
            else if (command == "alt" || command == "sweep") {
                const bool sweep = command == "sweep";
                in >> axis_name;
                if (axis_name == "off") (sweep ? sender.sweep_axis : sender.alt_axis) = -1;
                else if (const int axis = x4vr::opentrack::axis_index(axis_name); axis >= 0 && (in >> value) && (!sweep || ((in >> period) && period > 0))) {
                    if (sweep) { sender.sweep_axis = axis; sender.sweep_amplitude = value; sender.sweep_period = period; }
                    else { sender.alt_axis = axis; sender.alt_value = value; }
                } else ok = false;
            } else if (const int axis = x4vr::opentrack::axis_index(command); axis >= 0 && (in >> value)) sender.base.axis(axis) = value;
            else ok = false;
        }
        if (!ok) std::cout << "Didn't understand that. Type 'help'.\n";
        std::cout << describe(sender) << '\n';
    }
    running = false;
    thread.join();
    close(fd);
    std::cout << "\nStopped. " << describe(sender) << '\n';
    return 0;
}

// ---- check / fix-settings --------------------------------------------------------------------
// Linux X4 keeps one config per Steam account under ~/.config/EgoSoft/X4/<id>/; use the newest.
std::filesystem::path x4_config() {
    const char* home = std::getenv("HOME");
    const auto base = std::filesystem::path(home ? home : ".")/".config/EgoSoft/X4";
    std::filesystem::path best;
    std::filesystem::file_time_type newest{};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
        const auto config = entry.path()/"config.xml";
        const auto time = std::filesystem::last_write_time(config, error);
        if (!error && (best.empty() || time > newest)) { best = config; newest = time; }
    }
    return best;
}
bool x4_running() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        std::error_code link_error;
        if (std::filesystem::read_symlink(entry.path()/"exe", link_error).filename() == "X4") return true;
    }
    return false;
}
// The Windows launcher's checks (tools/launcher/launcher_settings.hpp, tested in launcher_tests),
// minus "fullscreen, not borderless": on Windows that is for NVIDIA DSR; on Linux X4's borderless
// window at the desktop resolution is fine.
// X4's resolution: the smallest common 16:9 mode at least as large as what SteamVR uses (saved by
// SteamVR directly when it runs, else saved by the mod in x4_resolution.txt while X4 ran), so X4
// doesn't render pixels SteamVR throws away. X4VR_RESOLUTION=WxH sets it, =0 leaves it alone.
std::filesystem::path settings_file();
std::filesystem::path settings_marker();
// What the mod computes at startup (vr_layer.cpp, presenter_initialize): X4's image spans
// 2*tan_x by 2*tan_y (tan_y = game_tan_y, X4's FOV), at the pixels per tangent SteamVR recommends.
// Asked as a background app: it doesn't start SteamVR or show up as a running game.
bool steamvr_resolution(const std::string& xml, int& w, int& h) {
    if (std::getenv("X4VR_NO_STEAMVR_QUERY")) return false;
    auto error = vr::VRInitError_None;
    auto* system = vr::VR_Init(&error, vr::VRApplication_Background);
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
    double tan_y = 0.8675; // stereo.txt game_tan_y
    std::ifstream settings(settings_file());
    for (std::string line; std::getline(settings, line);) if (line.rfind("game_tan_y=", 0) == 0) tan_y = std::atof(line.c_str()+11);
    std::string rw, rh;
    const double aspect = x4vr::launcher::xml_value(xml, "res_width", rw) && x4vr::launcher::xml_value(xml, "res_height", rh) &&
                          std::atoi(rh.c_str()) > 0 ? std::atof(rw.c_str())/std::atof(rh.c_str()) : 16.0/9;
    w = int(std::lround(2*tan_y*aspect*want_x));
    h = int(std::lround(2*tan_y*want_y));
    return w > 0 && h > 0;
}
std::pair<int, int> wanted_resolution(const std::string& xml) {
    int w = 0, h = 0;
    if (const char* set = std::getenv("X4VR_RESOLUTION"); set && *set) {
        if (std::sscanf(set, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) return {w, h};
        return {0, 0};
    }
    const auto saved = settings_file().parent_path()/"x4_resolution.txt";
    if (steamvr_resolution(xml, w, h)) {
        std::ofstream(saved, std::ios::trunc) << w << 'x' << h << '\n';
    } else { // SteamVR not running: what the mod saved last time
        std::ifstream in(saved);
        if (!(in >> w) || in.get() != 'x' || !(in >> h) || w <= 0 || h <= 0) return {0, 0};
    }
    static constexpr std::pair<int, int> modes[] = {{1920, 1080}, {2560, 1440}, {2880, 1620}, {3200, 1800}, {3840, 2160}};
    for (const auto& mode : modes) if (mode.first >= w && mode.second >= h) return mode;
    return modes[std::size(modes)-1];
}
std::vector<x4vr::launcher::Check> linux_checks(const std::string& xml) {
    const auto [width, height] = wanted_resolution(xml);
    auto checks = x4vr::launcher::check_x4(xml, width, height);
    std::erase_if(checks, [](const auto& c) { return c.label.rfind("Display mode", 0) == 0; }); // Windows' fullscreen rule
    // Linux X4 renders fullscreen and borderless windows at the desktop size whatever its
    // resolution setting says; only a window keeps it (tiling window managers may resize it).
    if (width > 0 && height > 0) {
        std::string fullscreen = "(missing)", borderless = "(missing)";
        x4vr::launcher::xml_value(xml, "fullscreen", fullscreen);
        x4vr::launcher::xml_value(xml, "borderless", borderless);
        checks.push_back({"Display mode: windowed", true, fullscreen == "false" && borderless == "false",
                          "fullscreen "+fullscreen+", borderless "+borderless, {{"fullscreen", "false"}, {"borderless", "false"}}});
    }
    return checks;
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
int check_settings(bool fix, bool automatic) {
    const auto path = x4_config();
    if (path.empty()) { std::cerr << "X4's config.xml not found under ~/.config/EgoSoft/X4 (start X4 once).\n"; return automatic ? 0 : 1; }
    const auto xml = read_text(path);
    const auto checks = linux_checks(xml);
    // Only settings that exist in this config.xml are changed: Linux X4 may name some differently,
    // and a key X4 doesn't know would only clutter the file.
    std::vector<x4vr::launcher::Check> fixable;
    for (const auto& c : checks) {
        std::string value;
        const bool present = std::all_of(c.fix.begin(), c.fix.end(), [&](const auto& kv) { return x4vr::launcher::xml_value(xml, kv.first, value); });
        if (!c.ok && present) fixable.push_back(c);
        if (!automatic) std::printf("%-4s %-36s %s%s\n", c.ok ? "ok" : c.required ? "FIX" : "tip", c.label.c_str(), c.current.c_str(),
                                    c.ok || present ? "" : "  (not in this config.xml: change it in the game)");
    }
    if (!fix) {
        if (!automatic) std::cout << (fixable.empty() ? "Nothing to fix." : "Run 'x4vr fix-settings' with X4 closed to fix these.") << '\n';
        return 0;
    }
    if (fixable.empty()) { if (!automatic) std::cout << "Nothing to fix.\n"; return 0; }
    if (x4_running()) { std::cerr << "X4 is running: close it first (it rewrites config.xml when it exits).\n"; return 1; }
    const auto backup = path.string()+".x4vr-backup";
    std::error_code error;
    if (!std::filesystem::exists(backup)) std::filesystem::copy_file(path, backup, error);
    const auto temporary = path.string()+".x4vr-tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << x4vr::launcher::fix_x4(xml, fixable);
        if (!out) { std::cerr << "Can't write " << temporary << '\n'; return 1; }
    }
    std::filesystem::rename(temporary, path);
    for (const auto& c : fixable) std::cout << "x4vr: X4 setting fixed for VR: " << c.label << " (was " << c.current << ")\n";
    std::cout << "x4vr: " << path.string() << " updated; original kept as " << backup << '\n';
    return 0;
}

// ---- hud --------------------------------------------------------------------------------------
// The Windows launcher's HUD distance mod (tools/launcher/hud_mod.hpp, launcher.cpp): the HUD's
// anchor positions and world-space scale factors times k, as a substitution extension.
std::filesystem::path game_dir() {
    if (const char* dir = std::getenv("X4VR_GAME_DIR"); dir && *dir) return dir;
    const char* home = std::getenv("HOME");
    const std::filesystem::path h = home ? home : ".";
    for (const auto& candidate : {h/".local/share/Steam/steamapps/common/X4 Foundations", h/".steam/steam/steamapps/common/X4 Foundations"})
        if (std::filesystem::exists(candidate/"01.cat")) return candidate;
    return {};
}
// Linux X4 9.00 ships each UI script as .lua and as .xpl, precompiled bytecode, and loads the
// .xpl: replacing only the .lua moved the HUD back but kept its size (2026-10-04). The extension
// therefore also puts the patched Lua source at the .xpl path (Lua's loader takes source or bytecode).
std::string xpl_of(const std::string& lua) { return lua.substr(0, lua.size()-4)+".xpl"; }
std::map<std::string, std::string> hud_originals(const std::filesystem::path& game) {
    std::set<std::string> paths(x4vr::launcher::hud_anchor_files().begin(), x4vr::launcher::hud_anchor_files().end());
    for (const auto& script : x4vr::launcher::hud_scripts()) { paths.insert(script); paths.insert(xpl_of(script)); }
    return x4vr::launcher::read_game_files(game, paths);
}
std::string source_hash(const std::map<std::string, std::string>& originals) {
    std::string all;
    for (const auto& [path, blob] : originals) all += path+x4vr::linux_port::md5_hex(blob);
    return x4vr::linux_port::md5_hex(all);
}
bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return bool(out);
}
// x4vr_hud.txt: scale=, source=
std::map<std::string, std::string> installed_hud(const std::filesystem::path& extension) {
    std::map<std::string, std::string> values;
    std::ifstream in(extension/"x4vr_hud.txt");
    for (std::string line; std::getline(in, line);)
        if (const auto split = line.find('='); split != std::string::npos) values[line.substr(0, split)] = line.substr(split+1);
    return values;
}
bool install_hud(const std::filesystem::path& game, double scale, std::string& error) {
    const auto originals = hud_originals(game);
    auto files = x4vr::launcher::hud_files(originals, scale, error);
    if (files.empty()) return false;
    for (const auto& script : x4vr::launcher::hud_scripts())
        if (originals.count(xpl_of(script))) files[xpl_of(script)] = files.at(script);
    const auto extension = game/"extensions/x4vr_hud";
    std::error_code ignored;
    std::filesystem::remove_all(extension, ignored);
    std::filesystem::create_directories(extension, ignored);
    std::ofstream data(extension/"subst_01.dat", std::ios::binary);
    std::string index;
    const auto stamp = std::to_string(std::time(nullptr));
    for (const auto& [path, blob] : files) {
        data.write(blob.data(), std::streamsize(blob.size()));
        index += path+' '+std::to_string(blob.size())+' '+stamp+' '+x4vr::linux_port::md5_hex(blob)+'\n';
    }
    data.close();
    const auto scale_text = x4vr::launcher::format_number(scale);
    const bool ok = data && write_text(extension/"subst_01.cat", index) &&
        write_text(extension/"content.xml", "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<content id=\"x4vr_hud\" name=\"X4 VR HUD distance\" version=\"100\" save=\"0\" enabled=\"1\"\n"
            "  description=\"Generated by x4vr: HUD "+scale_text+"x farther away at the same apparent size, for VR. "
            "Built from your own game files.\">\n</content>\n") &&
        write_text(extension/"x4vr_hud.txt", "scale="+scale_text+"\nsource="+source_hash(originals)+"\n");
    if (!ok) error = "could not write "+extension.string();
    return ok;
}
int hud(const std::vector<std::string_view>& args) {
    if (args.size() != 1) { usage(); return 2; }
    const auto game = game_dir();
    const bool refresh = args[0] == "--refresh";
    if (game.empty()) {
        if (refresh) return 0;
        std::cerr << "X4's game folder not found: set X4VR_GAME_DIR to the folder with 01.cat.\n";
        return 1;
    }
    const auto extension = game/"extensions/x4vr_hud";
    const auto installed = installed_hud(extension);
    const double installed_scale = installed.count("scale") ? std::atof(installed.at("scale").c_str()) : 0;
    if (args[0] == "status") {
        if (installed_scale > 0) std::cout << "HUD distance mod installed: factor " << installed.at("scale") << " (" << extension.string() << ")\n";
        else std::cout << "HUD distance mod not installed (X4's default HUD distance).\n";
        return 0;
    }
    if (x4_running()) { std::cerr << "X4 is running: close it first (it loads extensions at startup).\n"; return refresh ? 0 : 1; }
    std::error_code ignored;
    if (refresh) { // after a game update the mod would replace new game files with old copies
        if (installed_scale <= 0 || (installed.count("source") && installed.at("source") == source_hash(hud_originals(game)))) return 0;
        std::string error;
        if (install_hud(game, installed_scale, error)) { std::cout << "x4vr: HUD distance mod rebuilt for the updated game files\n"; return 0; }
        std::filesystem::remove_all(extension, ignored);
        std::cout << "x4vr: HUD distance mod removed: it no longer matches this X4 version (" << error << ")\n";
        return 0;
    }
    if (args[0] == "remove") { std::filesystem::remove_all(extension, ignored); std::cout << "HUD distance mod removed.\n"; return 0; }
    double scale = 0;
    if (!parse_number(args[0], scale) || scale < 1 || scale > 6) { std::cerr << "HUD distance: use a factor between 1 and 6 (2.5 is a good start).\n"; return 2; }
    std::string error;
    if (!install_hud(game, scale, error)) { std::cerr << "Could not build the HUD mod: " << error << '\n'; return 1; }
    std::cout << "HUD distance mod installed: factor " << x4vr::launcher::format_number(scale) << " (" << extension.string() << ")\n";
    std::cout << "X4 will report a modified game: online features are off, and saves made with the mod stay flagged.\n";
    std::cout << "X4's Protected UI Mode blocks the HUD's size factors: turn it off in X4 (Extension Settings), else the HUD only moves back and shrinks.\n";
    // X4's per-user content.xml (next to config.xml) remembers extensions turned off in its menu.
    // Outside a VR session the extension stays off: a VR launch turns it on (settings-mode).
    if (const auto config = x4_config(); !config.empty() && std::filesystem::exists(settings_marker())) {
        const auto content_path = config.parent_path()/"content.xml";
        bool disabled = false;
        const auto content = x4vr::launcher::enable_hud_extension(read_text(content_path), disabled);
        if (disabled && !write_text(content_path, content))
            std::cout << "X4 has it turned off: turn on \"X4 VR HUD distance\" in X4's Extensions menu.\n";
    }
    return 0;
}

// ---- game-grep --------------------------------------------------------------------------------
int game_grep(const std::vector<std::string_view>& args) {
    if (args.empty() || args.size() > 2) { usage(); return 2; }
    const auto game = game_dir();
    if (game.empty()) { std::cerr << "X4's game folder not found: set X4VR_GAME_DIR to the folder with 01.cat.\n"; return 1; }
    std::regex text, path_filter;
    try {
        text = std::regex(std::string(args[0]));
        path_filter = std::regex(args.size() > 1 ? std::string(args[1]) : std::string(R"(^(ui/|assets/ui/).*\.(lua|xpl|xml)$)"));
    } catch (const std::regex_error& error) { std::cerr << "Bad regex: " << error.what() << '\n'; return 2; }
    // Every catalog's index, later catalogs replacing earlier copies of a path (as read_game_files).
    std::set<std::string> paths;
    for (int index = 1; index <= 99; ++index) {
        char name[8];
        std::snprintf(name, sizeof(name), "%02d.cat", index);
        std::ifstream cat(game/name);
        for (std::string line; std::getline(cat, line);) {
            int spaces = 0; size_t cut = line.size();
            while (spaces < 3 && cut != std::string::npos && cut > 0) { cut = line.rfind(' ', cut-1); ++spaces; }
            if (spaces == 3 && cut != std::string::npos) {
                auto path = line.substr(0, cut);
                if (std::regex_search(path, path_filter)) paths.insert(std::move(path));
            }
        }
    }
    int matches = 0;
    for (const auto& [path, blob] : x4vr::launcher::read_game_files(game, paths)) {
        std::istringstream in(blob);
        int number = 0;
        for (std::string line; std::getline(in, line);) {
            ++number;
            if (std::regex_search(line, text)) { std::cout << path << ':' << number << ": " << line << '\n'; ++matches; }
        }
    }
    std::cerr << matches << " matches in " << paths.size() << " files\n";
    return 0;
}

// ---- ctl ------------------------------------------------------------------------------------
std::filesystem::path settings_file() {
    if (const char* dir = std::getenv("X4VR_DIR"); dir && *dir) return std::filesystem::path(dir)/"stereo.txt";
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) return std::filesystem::path(state)/"x4vr/stereo.txt";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".")/".local/state/x4vr/stereo.txt";
}
int ctl(const std::vector<std::string_view>& args) {
    if (args.size() != 1 || (args[0] != "recenter" && args[0] != "flat")) { usage(); return 2; }
    const auto path = settings_file();
    const int next = x4vr::linux_port::control_settings(path, args[0]);
    if (next < 0) { std::cerr << "Can't update " << path << " (start X4 once with x4vr-run)\n"; return 1; }
    std::cout << (args[0] == "recenter" ? "recentred" : next == 2 ? "flat screen on" : "flat screen automatic") << '\n';
    return 0;
}

// ---- elf-classes ----------------------------------------------------------------------------
int elf_classes(const std::vector<std::string_view>& args) {
    if (args.size() != 2) { usage(); return 2; }
    const auto image = x4vr::elf::Image::load(std::string(args[0]));
    const auto classes = x4vr::elf::find_classes(image, args[1]);
    for (const auto& c : classes) {
        std::printf("class %s  [%s]  type_info 0x%llx\n", c.name.c_str(), c.mangled.c_str(), static_cast<unsigned long long>(c.typeinfo));
        for (const auto& base : c.bases) std::printf("  base %s\n", base.c_str());
        if (c.vtables.empty()) std::puts("  no vtable found");
        for (const auto& v : c.vtables) {
            std::printf("  vtable address_point 0x%llx  offset_to_top %lld  %zu slots\n",
                        static_cast<unsigned long long>(v.address_point()), static_cast<long long>(v.offset_to_top), v.slots.size());
            for (size_t i = 0; i < v.slots.size(); ++i)
                std::printf("    [%3zu] +0x%03zx  0x%llx\n", i, i*8, static_cast<unsigned long long>(v.slots[i]));
        }
    }
    std::printf("%zu classes matching \"%.*s\"\n", classes.size(), int(args[1].size()), args[1].data());
    return classes.empty() ? 3 : 0;
}

// ---- menu (x4vr without arguments): checks, settings, launch in VR, bug report, uninstall ----
// The Steam launch option `x4vr-run %command%` stays set; x4vr-run starts VR only when a launch
// request from here is fresh (linux/x4vr-run.in), so Steam's Play button starts the normal game.
// `steam -applaunch` asks the running Steam client to start X4 with Steam's usual chain
// (reaper, the Steam Linux Runtime); the game inherits Steam's environment, not ours.
std::filesystem::path program; // this executable as started (argv[0], symlinks unresolved)
std::filesystem::path state_dir() { return settings_file().parent_path(); }
std::filesystem::path run_script() { return program.parent_path()/"x4vr-run"; }
std::filesystem::path desktop_file() {
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    return (data && *data ? std::filesystem::path(data) : std::filesystem::path(home ? home : ".")/".local/share")/"applications/x4vr.desktop";
}
bool process_named(std::string_view name) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        std::ifstream comm(entry.path()/"comm");
        std::string text;
        if (comm && std::getline(comm, text) && text == name) return true;
    }
    return false;
}
bool steamvr_running() { return process_named("vrserver"); }
std::string quoted_path(const std::filesystem::path& path) {
    const auto text = path.string();
    return text.find(' ') == std::string::npos ? text : "\""+text+"\"";
}
std::string wanted_launch_option() { return quoted_path(run_script())+" %command%"; }

// Starts a program detached from this terminal (its own session, output discarded).
bool spawn(const std::vector<std::string>& argv) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return true;
}
// Runs a program and waits; its output goes to `log`. True on exit status 0.
bool run_and_wait(const std::vector<std::string>& argv, const std::filesystem::path& log) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        const int out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        const int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// X4's launch option in every Steam account: 0 none set, 1 ours, 2 an x4vr-run elsewhere (an
// older build), 3 something else; `accounts` = how many accounts know X4.
struct LaunchOption { int state = 0; int accounts = 0; std::string value; };
LaunchOption launch_option() {
    LaunchOption result;
    for (const auto& file : x4vr::steam::local_configs()) {
        const auto value = x4vr::steam::launch_options(x4vr::steam::read_file(file));
        if (!value) continue;
        ++result.accounts;
        const int state = value->empty() ? 0 : *value == wanted_launch_option() ? 1 : value->find("x4vr-run") != std::string::npos ? 2 : 3;
        if (state > result.state) { result.state = state; result.value = *value; }
    }
    return result;
}
// Copies text to the clipboard: wl-copy (Wayland), xclip or xsel (X11), else the terminal's
// own clipboard (OSC 52, which most terminals support). Returns how.
std::string copy_to_clipboard(const std::string& text) {
    for (const auto& tool : std::vector<std::vector<std::string>>{{"wl-copy"}, {"xclip", "-selection", "clipboard"}, {"xsel", "--clipboard", "--input"}}) {
        int fds[2];
        if (pipe(fds) != 0) break;
        const pid_t child = fork();
        if (child == 0) {
            dup2(fds[0], 0);
            close(fds[0]); close(fds[1]);
            const int null = ::open("/dev/null", O_WRONLY);
            if (null >= 0) { dup2(null, 1); dup2(null, 2); }
            std::vector<char*> args;
            for (const auto& a : tool) args.push_back(const_cast<char*>(a.c_str()));
            args.push_back(nullptr);
            execvp(args[0], args.data());
            _exit(127);
        }
        close(fds[0]);
        if (child > 0) { (void)!::write(fds[1], text.data(), text.size()); }
        close(fds[1]);
        int status = 0;
        if (child > 0) waitpid(child, &status, 0);
        if (child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) return tool[0];
    }
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

// X4's settings for 2D and for VR, kept apart: launching in VR saves config.xml as
// config.xml.x4vr-2d and puts the VR copy (config.xml.x4vr-vr, if any) in its place; when X4
// exits, config.xml goes back to x4vr-vr and the 2D copy returns. So Steam's Play keeps the
// player's own settings, and VR keeps its own (x4vr-run, then fix-settings on top). A marker
// (x4-settings.vr: the config path) survives a crash: the next start of either kind restores 2D.
std::filesystem::path settings_marker() { return state_dir()/"x4-settings.vr"; }
bool copy_over(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    const auto temporary = to.string()+".x4vr-tmp";
    std::filesystem::copy_file(from, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if (!error) std::filesystem::rename(temporary, to, error);
    return !error;
}
// The HUD distance extension only in VR: X4's per-user content.xml records it as
// <extension id="x4vr_hud" enabled="..."/>; switched off for 2D (no farther HUD, no "modified"
// flag on 2D saves) and on for VR. Without an entry yet (X4 adds it once it has seen the
// extension) nothing changes.
void hud_extension_enabled(const std::filesystem::path& config, bool on) {
    const auto path = config.parent_path()/"content.xml";
    const auto text = read_text(path);
    const auto changed = std::regex_replace(text, std::regex(R"re((<extension\s+id="x4vr_hud"\s+enabled=")(true|false)")re"),
                                            std::string("$1")+(on ? "true" : "false")+"\"");
    if (changed != text) write_text(path, changed);
}
int settings_mode(const std::vector<std::string_view>& args) {
    if (args.size() != 1 || (args[0] != "vr" && args[0] != "2d" && args[0] != "status")) { usage(); return 2; }
    const auto marker = settings_marker();
    std::filesystem::path config = read_text(marker);
    while (!config.empty() && std::isspace(static_cast<unsigned char>(config.string().back()))) config = config.string().substr(0, config.string().size()-1);
    const bool in_vr = !config.empty();
    if (config.empty()) config = x4_config();
    if (args[0] == "status") { std::cout << (in_vr ? "X4 has its VR settings (" : "X4 has its 2D settings (") << config.string() << ")\n"; return 0; }
    if (config.empty()) return 0; // X4 never started: nothing to keep apart
    for (int i = 0; i < 20 && x4_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(500)); // X4 finishing its exit
    if (x4_running()) { std::cerr << "X4 is running: its settings are switched when it isn't.\n"; return 1; }
    const std::filesystem::path two_d = config.string()+".x4vr-2d", vr = config.string()+".x4vr-vr";
    std::error_code error;
    if (args[0] == "vr") {
        if (in_vr) return 0; // already (a launch that didn't get to restore 2D)
        if (!copy_over(config, two_d)) { std::cerr << "x4vr: can't save X4's 2D settings to " << two_d << '\n'; return 1; }
        if (std::filesystem::exists(vr) && !copy_over(vr, config)) { std::cerr << "x4vr: can't load X4's VR settings\n"; return 1; }
        std::filesystem::create_directories(marker.parent_path(), error);
        write_text(marker, config.string()+"\n");
        hud_extension_enabled(config, true);
        std::cout << "x4vr: X4's 2D settings saved (" << two_d.filename().string() << "), VR settings in place\n";
        return 0;
    }
    if (!in_vr) return 0;
    if (!copy_over(config, vr)) { std::cerr << "x4vr: can't save X4's VR settings to " << vr << '\n'; return 1; }
    if (std::filesystem::exists(two_d) && !copy_over(two_d, config)) { std::cerr << "x4vr: can't restore X4's 2D settings\n"; return 1; }
    std::filesystem::remove(marker, error);
    hud_extension_enabled(config, false);
    std::cout << "x4vr: X4's VR settings saved (" << vr.filename().string() << "), 2D settings restored\n";
    return 0;
}
std::string install_desktop() {
    const auto file = desktop_file();
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    const auto exec = quoted_path(program);
    const bool ok = write_text(file, "[Desktop Entry]\nType=Application\nName=X4 VR\n"
        "Comment=X4: Foundations in VR: checks, settings and launch\nExec="+exec+"\nTerminal=true\n"
        "Icon=steam_icon_392160\nCategories=Game;\n");
    return ok ? "Desktop entry written: "+file.string()+" (rofi -show drun: \"X4 VR\")" : "Can't write "+file.string();
}

// What the menu shows; the X4 scan (a second on the file) only once.
struct Checks {
    bool steam{}, steamvr{}, x4{};
    LaunchOption option;
    double hud{};            // installed HUD factor, 0: none
    bool hud_off_in_x4{};    // installed but switched off in X4's Extensions menu
    int settings_to_fix = -1; // -1: X4's config.xml not found, -2: VR settings made at the first VR launch
    bool desktop{};
};
Checks current_checks() {
    Checks c;
    c.steam = x4vr::steam::steam_running();
    c.steamvr = steamvr_running();
    c.x4 = x4_running();
    c.option = launch_option();
    if (const auto game = game_dir(); !game.empty()) {
        const auto installed = installed_hud(game/"extensions/x4vr_hud");
        c.hud = installed.count("scale") ? std::atof(installed.at("scale").c_str()) : 0;
    }
    if (const auto config = x4_config(); !config.empty()) {
        bool disabled = false;
        x4vr::launcher::enable_hud_extension(read_text(config.parent_path()/"content.xml"), disabled);
        c.hud_off_in_x4 = c.hud > 0 && disabled && std::filesystem::exists(settings_marker()); // off outside VR on purpose
        // config.xml holds the 2D settings except during a VR session; the VR ones are the copy.
        const auto vr = std::filesystem::path(config.string()+".x4vr-vr");
        const bool in_vr = std::filesystem::exists(settings_marker());
        const auto xml = read_text(in_vr ? config : vr);
        c.settings_to_fix = in_vr || std::filesystem::exists(vr) ? 0 : -2;
        if (c.settings_to_fix == 0) for (const auto& check : linux_checks(xml)) {
            std::string value;
            const bool present = std::all_of(check.fix.begin(), check.fix.end(), [&](const auto& kv) { return x4vr::launcher::xml_value(xml, kv.first, value); });
            if (!check.ok && check.required && present) ++c.settings_to_fix;
        }
    }
    c.desktop = std::filesystem::exists(desktop_file());
    return c;
}
std::string scan_summary(const x4vr::linux_port::code::X4Sites& sites, int& state) {
    const bool all = sites.backward_clamp && sites.onfoot_zeroing && sites.camera_offset && sites.frame_half_global && sites.opentrack_vtable;
    const bool core = sites.opentrack_vtable && sites.frame_half_global;
    state = all ? 0 : core ? 1 : 2;
    return all ? "all VR features supported" : core ? "some features off in this X4 (see Bug report)" : "this X4 build isn't supported";
}

// Runs `body` with std::cout and std::cerr captured (functions shared with the command line
// print); returns what they printed, one entry per line.
std::vector<std::string> captured(const std::function<void()>& body) {
    std::ostringstream text;
    auto* out = std::cout.rdbuf(text.rdbuf());
    auto* err = std::cerr.rdbuf(text.rdbuf());
    try { body(); } catch (const std::exception& e) { text << e.what() << '\n'; }
    std::cout.rdbuf(out);
    std::cerr.rdbuf(err);
    std::vector<std::string> lines;
    std::istringstream in(text.str());
    for (std::string line; std::getline(in, line);) if (!line.empty()) lines.push_back(line);
    return lines;
}

// Launch in VR: SteamVR (started if needed, waiting for it), then the request for x4vr-run and
// `steam -applaunch`. `progress` shows a line; `cancelled` is polled while waiting.
bool launch_vr(const std::function<void(const std::string&)>& progress, const std::function<bool()>& cancelled) {
    if (x4_running()) { progress("X4 is already running."); return false; }
    const auto option = launch_option();
    if (option.state != 1 && !(option.state == 2 && std::getenv("X4VR_ANY_RUN"))) {
        progress(option.accounts == 0 ? "Start X4 once from Steam first, then set its launch option."
                 : option.state == 2 ? "X4's launch option points to another x4vr-run: copy the new one (menu: Copy the launch option)."
                 : "Set X4's launch option first (menu: Copy the launch option).");
        return false;
    }
    if (!steamvr_running()) {
        progress("Starting SteamVR... (put the headset on and connect it)");
        spawn({"steam", "steam://rungameid/250820"});
        for (int i = 0; i < 180 && !steamvr_running(); ++i) {
            if (cancelled()) { progress("Cancelled."); return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!steamvr_running()) { progress("SteamVR didn't start within 90 s; start it from Steam and try again."); return false; }
        progress("SteamVR is running.");
    }
    std::error_code error;
    std::filesystem::create_directories(state_dir(), error);
    if (!write_text(state_dir()/"launch.request", std::to_string(std::time(nullptr))+"\n")) { progress("Can't write the launch request."); return false; }
    progress("Starting X4 through Steam...");
    spawn({"steam", "-applaunch", std::string(x4vr::steam::x4_app)});
    for (int i = 0; i < 240 && !x4_running(); ++i) {
        if (cancelled()) { progress("Stopped waiting; X4 may still start."); return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    progress(x4_running() ? "X4 is running in VR. Recentre: Ctrl+F12 or SteamVR's menu." : "X4 didn't start within 2 minutes; check Steam.");
    return x4_running();
}

// Bug report: logs, settings, X4's config.xml and a summary in ~/x4vr-report-<time>.tar.gz.
std::string make_report() {
    setenv("X4VR_NO_STEAMVR_QUERY", "1", 1); // the settings check: no SteamVR client from here
    char stamp[32];
    const auto now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const char* home = std::getenv("HOME");
    const auto out = std::filesystem::path(home ? home : ".")/("x4vr-report-"+std::string(stamp)+".tar.gz");
    const auto staging = std::filesystem::temp_directory_path()/("x4vr-report-"+std::string(stamp));
    std::error_code error;
    std::filesystem::create_directories(staging, error);
    for (const auto* name : {"x4vr.log", "x4vr.previous.log", "stderr.log", "stderr.previous.log", "stereo.txt", "x4_resolution.txt", "pair_stats.txt"})
        std::filesystem::copy_file(state_dir()/name, staging/name, error), error.clear();
    if (const auto config = x4_config(); !config.empty()) std::filesystem::copy_file(config, staging/"x4-config.xml", error), error.clear();
    std::ostringstream summary;
    summary << "x4vr report " << stamp << "\n";
    struct utsname u{};
    if (uname(&u) == 0) summary << "system: " << u.sysname << ' ' << u.release << ' ' << u.machine << "\n";
    const auto c = current_checks();
    summary << "steam running: " << c.steam << "\nsteamvr running: " << c.steamvr << "\nx4 running: " << c.x4
            << "\nlaunch option: " << c.option.value << " (state " << c.option.state << ")\nhud factor: " << c.hud
            << (c.hud_off_in_x4 ? " (off in X4)" : "") << "\nx4 settings to fix: " << c.settings_to_fix << "\n";
    if (const auto game = game_dir(); !game.empty() && std::filesystem::exists(game/"X4")) {
        summary << "x4 scan (" << (game/"X4").string() << "):\n";
        for (const auto& note : x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load((game/"X4").string())).notes) summary << "  " << note << "\n";
    }
    write_text(staging/"summary.txt", summary.str());
    const bool ok = run_and_wait({"tar", "czf", out.string(), "-C", staging.string(), "."}, staging.parent_path()/"x4vr-report-tar.log");
    std::filesystem::remove_all(staging, error);
    return ok ? out.string() : std::string();
}
constexpr const char* issues_url = "https://github.com/Cully-Curwen/X4_VR_Linux/issues/new";

// Uninstall: what the mod left, each as a choice.
struct Removal { std::string id, label; bool on; std::string help; };
std::vector<Removal> removals() {
    return {
        {"desktop", "Remove the desktop entry", true, desktop_file().string()},
        {"hud", "Remove the HUD distance extension", true, "X4 must be closed. Saves made with it stay flagged as modified."},
        {"restore_x4", "Restore X4's settings from before the mod", false, "config.xml.x4vr-backup, the first copy: also undoes 2D settings changed since."},
        {"x4_copies", "Delete the mod's copies of X4's settings", true, "config.xml.x4vr-2d, -vr and -backup. Your 2D settings stay in config.xml."},
        {"state", "Delete the mod's settings and logs", true, state_dir().string()},
    };
}
std::vector<std::string> uninstall(const std::vector<Removal>& chosen) {
    std::vector<std::string> done;
    const auto on = [&](std::string_view id) { return std::any_of(chosen.begin(), chosen.end(), [&](const auto& r) { return r.id == id && r.on; }); };
    std::error_code error;
    if (launch_option().state != 0) done.push_back("Clear X4's launch option in Steam: X4 > Properties > General > Launch options.");
    if (on("desktop")) { std::filesystem::remove(desktop_file(), error); done.push_back("Desktop entry removed."); }
    if ((on("hud") || on("restore_x4")) && x4_running()) done.push_back("X4 is running: close it to remove the HUD extension or restore its settings.");
    else {
        if (on("hud")) {
            if (const auto game = game_dir(); !game.empty()) std::filesystem::remove_all(game/"extensions/x4vr_hud", error);
            if (const auto config = x4_config(); !config.empty()) { // and X4's own record of it
                const auto content = config.parent_path()/"content.xml";
                const auto text = read_text(content);
                const auto cleaned = std::regex_replace(text, std::regex(R"re([ \t]*<extension\s+id="x4vr_hud"[^>]*/>[ \t]*\r?\n?)re"), "");
                if (cleaned != text) write_text(content, cleaned);
            }
            done.push_back("HUD distance extension removed.");
        }
        captured([] { settings_mode(std::vector<std::string_view>{"2d"}); }); // a VR session that didn't finish
        if (const auto config = x4_config(); !config.empty()) {
            const auto backup = config.string()+".x4vr-backup";
            if (on("restore_x4") && std::filesystem::exists(backup))
                done.push_back(copy_over(backup, config) ? "X4's settings restored from before the mod." : "Can't restore X4's settings.");
            if (on("x4_copies")) {
                for (const auto* suffix : {".x4vr-backup", ".x4vr-2d", ".x4vr-vr"}) std::filesystem::remove(config.string()+suffix, error);
                done.push_back("The mod's copies of X4's settings deleted.");
            }
        }
    }
    if (on("state")) { std::filesystem::remove_all(state_dir(), error); done.push_back("Mod settings and logs deleted."); }
    done.push_back("Last: delete the mod's folder (its code and build).");
    return done;
}

// The VR settings in stereo.txt (re-read by the mod every half second, so changes apply live).
void settings_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    const auto path = settings_file();
    if (!std::filesystem::exists(path)) { // first use: the defaults, as x4vr-run would copy them
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::copy_file(program.parent_path().parent_path()/"share/x4vr/stereo.txt", path, error);
    }
    const auto get = [&](const char* key, const char* fallback) { return x4vr::linux_port::read_setting(path, key, fallback); };
    Menu menu(terminal);
    menu.title = "X4 VR: settings ("+path.string()+")";
    for (;;) {
        const int theater = std::atoi(get("theater", "1").c_str());
        std::vector<Item> items{
            heading("VR settings (apply at once, also while X4 runs)"),
            toggle("stereo", "3D (stereo)", get("stereo", "1") != "0", "Off: the same image for both eyes."),
            toggle("shared_pose", "Shared pose per eye pair", get("shared_pose", "1") != "0",
                   "Stops right-eye ghosting on headsets whose link uses one pose for both eyes (Steam Frame)."),
            toggle("cursor", "Mouse cursor in VR", get("cursor", "1") != "0"),
            choice("theater", "Flat screen for menus", {"automatic", "always", "never"}, theater == 2 ? 1 : theater == 0 ? 2 : 0,
                   "Automatic: menus and views without ship controls go to a flat screen in VR."),
            number("ipd_scale", "World scale", std::atof(get("ipd_scale", "1").c_str()), 0.05, 0.3, 3, 2,
                   "Eye distance: smaller makes the world look bigger. ←→ or Enter to type."),
            number("theater_distance", "Flat screen distance (m)", std::atof(get("theater_distance", "2").c_str()), 0.25, 0.5, 10, 2),
            number("theater_width", "Flat screen width (m)", std::atof(get("theater_width", "2.2").c_str()), 0.1, 0.5, 10, 2),
            heading(""),
            action("back", "Back"),
        };
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || (e.kind == Event::Activate && e.id == "back")) return;
        if (e.kind != Event::Changed) continue;
        std::string value;
        if (e.item.kind == Item::Toggle) value = e.item.on ? "1" : "0";
        else if (e.item.kind == Item::Choice) value = e.item.choice == 0 ? "1" : e.item.choice == 1 ? "2" : "0";
        else value = format(e.item.number, e.item.decimals);
        if (!x4vr::linux_port::write_setting(path, e.id, value)) menu.messages.push_back("Can't write "+path.string());
    }
}
void uninstall_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    auto chosen = removals();
    Menu menu(terminal);
    menu.title = "X4 VR: uninstall";
    for (;;) {
        std::vector<Item> items{heading("Remove what the mod set up (tick with Space)")};
        for (const auto& r : chosen) items.push_back(toggle(r.id, r.label, r.on, r.help));
        items.push_back(heading(""));
        items.push_back(action("go", "Remove the ticked items"));
        items.push_back(action("back", "Back"));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || (e.kind == Event::Activate && e.id == "back")) return;
        if (e.kind == Event::Changed)
            for (auto& r : chosen) if (r.id == e.id) r.on = e.item.on;
        if (e.kind == Event::Activate && e.id == "go") menu.messages = uninstall(chosen);
    }
}
int menu(std::string_view start = {}) {
    using namespace x4vr::tui;
    setenv("X4VR_NO_STEAMVR_QUERY", "1", 1); // the settings checks: no SteamVR client from here
    Terminal terminal;
    if (!terminal.ok()) { std::cerr << "x4vr: the menu needs a terminal (run it in one, or use the subcommands: x4vr help)\n"; return 1; }
    if (start == "uninstall") { uninstall_screen(terminal); return 0; }
    Menu menu(terminal);
    menu.title = "X4 VR for Linux";
    menu.timeout_ms = 2000; // live status (SteamVR, X4)
    menu.messages = {"Checking X4..."};
    {
        std::vector<Item> none;
        Event e;
        menu.timeout_ms = 0;
        menu.step(none, e);
        menu.timeout_ms = 2000;
    }
    x4vr::linux_port::code::X4Sites sites;
    bool scanned = false;
    if (const auto game = game_dir(); !game.empty() && std::filesystem::exists(game/"X4")) {
        try { sites = x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load((game/"X4").string())); scanned = true; } catch (...) {}
    }
    menu.messages.clear();
    double hud_choice = -1;
    for (;;) {
        const auto c = current_checks();
        if (hud_choice < 0) hud_choice = c.hud > 0 ? c.hud : 2.5;
        int scan_state = 3;
        const auto scan_text = scanned ? scan_summary(sites, scan_state) : std::string("X4 not found (set X4VR_GAME_DIR)");
        std::vector<Item> items{
            heading("Status"),
            status("Steam", c.steam ? 0 : 1, c.steam ? "running" : "not running (Launch starts it)"),
            status("SteamVR", c.steamvr ? 0 : 1, c.steamvr ? "running" : "not running (Launch starts it)"),
            status("X4 build", scan_state, scan_text),
            status("Steam launch option", c.option.state == 1 ? 0 : 2,
                   c.option.state == 1 ? "set" : c.option.state == 2 ? "points to another x4vr-run" : c.option.state == 3 ? "set to something else: "+c.option.value
                   : c.option.accounts ? "not set (Copy the launch option)" : "start X4 once from Steam first"),
            status("X4 settings for VR", c.settings_to_fix < 0 ? 3 : c.settings_to_fix ? 1 : 0,
                   c.settings_to_fix == -1 ? "config.xml not found (start X4 once)" : c.settings_to_fix == -2 ? "made at the first VR launch"
                   : c.settings_to_fix ? std::to_string(c.settings_to_fix)+" to fix (fixed at launch)" : "ok (2D settings kept apart)",
                   "VR uses its own copy of X4's settings (config.xml.x4vr-vr); Steam's Play keeps yours."),
            status("HUD distance", c.hud_off_in_x4 ? 1 : 0, c.hud > 0 ? x4vr::launcher::format_number(c.hud)+"x"+(c.hud_off_in_x4 ? ", but off in X4's Extensions menu" : "") : "off (X4's default)"),
            status("X4", c.x4 ? 0 : 3, c.x4 ? "running" : "not running"),
            heading(""),
            heading("Play"),
        };
        if (!c.x4) items.push_back(action("launch", "Launch X4 in VR", "Starts SteamVR if needed, then X4 through Steam with the mod. Steam's Play button starts the normal game."));
        else {
            items.push_back(action("recenter", "Recentre the view", "Look straight ahead first. Same as Ctrl+F12 in X4, or SteamVR's recentre."));
            items.push_back(action("flat", "Flat screen on/off", "Same as Ctrl+F11 in X4."));
        }
        items.push_back(action("settings", "VR settings...", "World scale, flat screen, cursor, stereo."));
        items.push_back(number("hud_factor", "HUD distance factor", hud_choice, 0.5, 1, 6, 1, "←→ then \"Apply HUD distance\". 2.5: the HUD 2.5x farther at the same size. X4 must be closed."));
        items.push_back(action("hud_apply", "Apply HUD distance"));
        if (c.hud > 0) items.push_back(action("hud_remove", "Remove HUD distance (X4's default)"));
        items.push_back(heading(""));
        items.push_back(heading("Setup"));
        items.push_back(action("option_copy", "Copy the launch option", "Then paste it in Steam: X4 > Properties > Launch options. Steam's Play still starts the normal game."));
        if (!c.desktop) items.push_back(action("desktop", "Add to the app launcher (rofi, desktop menus)", desktop_file().string()));
        items.push_back(action("report", "Make a bug report", "Packs logs, settings and a summary into ~/x4vr-report-<time>.tar.gz."));
        items.push_back(action("uninstall", "Uninstall...", "Remove the launch option, desktop entry, HUD extension, settings."));
        items.push_back(action("quit", "Quit"));
        Event e;
        if (!menu.step(items, e)) continue;
        if (e.kind == Event::Back || e.id == "quit") return 0;
        if (e.kind == Event::Changed && e.id == "hud_factor") { hud_choice = e.item.number; continue; }
        if (e.kind != Event::Activate) continue;
        if (e.id == "launch") {
            std::vector<Item> progress_items{heading("Launch in VR"), text("Esc: stop waiting")};
            Menu progress(terminal);
            progress.title = menu.title;
            progress.timeout_ms = 0;
            std::vector<std::string> lines;
            const auto show = [&](const std::string& line) { lines.push_back(line); progress.messages = lines; Event ignored; progress.step(progress_items, ignored); };
            const auto stop = [&] { return terminal.key(0) == Escape; };
            launch_vr(show, stop); // x4vr-run switches X4 to its VR settings, fixes them and refreshes the HUD
            menu.messages = lines;
        } else if (e.id == "recenter" || e.id == "flat") {
            const int next = x4vr::linux_port::control_settings(settings_file(), e.id);
            menu.messages = {next < 0 ? "Can't update "+settings_file().string() : e.id == "recenter" ? "Recentred." : next == 2 ? "Flat screen on." : "Flat screen automatic."};
        } else if (e.id == "settings") settings_screen(terminal);
        else if (e.id == "hud_apply") {
            const auto factor = x4vr::launcher::format_number(hud_choice);
            menu.messages = captured([&] { hud(std::vector<std::string_view>{factor}); });
        } else if (e.id == "hud_remove") menu.messages = captured([] { hud(std::vector<std::string_view>{"remove"}); });
        else if (e.id == "option_copy") {
            const auto how = copy_to_clipboard(wanted_launch_option());
            menu.messages = {"Copied (" + how + "): " + wanted_launch_option(), launch_option_steps(), "Steam picks it up at the next launch."};
        }
        else if (e.id == "desktop") menu.messages = {install_desktop()};
        else if (e.id == "report") {
            menu.messages = {"Packing the report..."};
            const auto file = make_report();
            menu.messages = file.empty() ? std::vector<std::string>{"Couldn't write the report (tar missing?)."}
                                         : std::vector<std::string>{"Report: "+file, "Attach it to a new issue: "+std::string(issues_url)};
        } else if (e.id == "uninstall") uninstall_screen(terminal);
    }
}
// The menu's actions as commands, for scripts.
int launch_command() {
    return launch_vr([](const std::string& line) { std::cout << line << '\n'; }, [] { return false; }) ? 0 : 1;
}
int launch_option_command(const std::vector<std::string_view>& args) {
    if (args.size() != 1) { usage(); return 2; }
    if (args[0] == "status") {
        const auto o = launch_option();
        std::cout << (o.accounts == 0 ? "No Steam account here has started X4 yet.\n"
                      : o.state == 1 ? "Set: "+o.value+"\n" : o.state == 0 ? "Not set.\n" : "Set to: "+o.value+" (wanted: "+wanted_launch_option()+")\n");
        return o.state == 1 ? 0 : 1;
    }
    if (args[0] != "copy") { usage(); return 2; }
    const auto how = copy_to_clipboard(wanted_launch_option());
    std::cout << "Copied (" << how << "): " << wanted_launch_option() << '\n' << launch_option_steps() << '\n';
    return 0;
}
}

// The X4 scan the mod runs at startup (code_scan.hpp), on the executable file.
int patterns(const std::vector<std::string_view>& args) {
    if (args.size() > 1) { usage(); return 2; }
    const std::filesystem::path path = args.empty() ? game_dir()/"X4" : std::filesystem::path(args[0]);
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
        if (first.find('/') != std::string::npos) program = std::filesystem::absolute(first, error);
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
        if (command == "uninstall") return menu("uninstall");
        if (command == "launch" && args.empty()) return launch_command();
        if (command == "launch-option") return launch_option_command(args);
        if (command == "settings-mode") return settings_mode(args);
        if (command == "install-desktop" && args.empty()) { std::cout << install_desktop() << '\n'; return 0; }
        if (command == "report" && args.empty()) {
            const auto file = make_report();
            if (file.empty()) { std::cerr << "Couldn't write the report (tar missing?)\n"; return 1; }
            std::cout << "Report: " << file << "\nAttach it to a new issue: " << issues_url << '\n';
            return 0;
        }
        if (command == "vr-check") return vr_check(args);
        if (command == "udp-send") return udp_send(args);
        if (command == "elf-classes") return elf_classes(args);
        if (command == "ctl") return ctl(args);
        if (command == "hud") return hud(args);
        if (command == "game-grep") return game_grep(args);
        if (command == "patterns") return patterns(args);
        if (command == "check" && args.empty()) return check_settings(false, false);
        if (command == "fix-settings" && args.size() <= 1 && (args.empty() || args[0] == "--auto"))
            return check_settings(true, !args.empty());
        if (command == "help" || command == "--help" || command == "-h") { usage(); return 0; }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "x4vr " << command << ": " << error.what() << '\n';
        return 1;
    }
}
