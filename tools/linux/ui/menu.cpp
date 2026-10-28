// The launcher's menu: x4vr with no command. One screen in sections (status, play, notices, VR
// settings, X4's settings for VR, setup), and the screens it opens: the in-game settings checklist,
// tiling window manager rules, uninstall.

#include "ui/menu.hpp"
#include "../launcher/launcher_settings.hpp"
#include "build_info.h"
#include "code_scan.hpp"
#include "common.hpp"
#include "elf_classes.hpp"
#include "hud.hpp"
#include "install.hpp"
#include "launch.hpp"
#include "report.hpp"
#include "settings_control.hpp"
#include "status.hpp"
#include "ui/profiles.hpp"
#include "ui/terminal_ui.hpp"
#include "x4.hpp"
#include "x4_settings.hpp"
#include "x4_settings_rules.hpp"
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace x4vr::linux_launcher {

// Copies text to the clipboard: wl-copy (Wayland), xclip or xsel (X11), else the terminal's
// own clipboard (OSC 52, which most terminals support). Returns how.
static std::string copy_to_clipboard(const std::string& text) {
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
static std::string get_launch_option_steps() { return "In Steam: X4 > Properties > General > Launch options, paste it there."; }

// Notices: known issues, tips and requests for help, shipped in share/x4vr/notices.txt.
// One per line, "issue: text", "tip: text" or "help: text"; # starts a comment.
static std::vector<std::pair<int, std::string>> read_notices() {
    std::vector<std::pair<int, std::string>> list;
    std::ifstream in(get_data_dir()/"notices.txt");
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
static std::optional<x4vr::launcher::Stats> read_headset_stats() {
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

// Tiling window managers: rules that float the VR window.
// X4 renders at its window's size, and a tiling window manager resizes the window (and honours
// X4's fullscreen request, which shrinks it to the monitor). In VR the window's class is X4VR
// (x4vr-run sets SDL_APP_ID), so a rule can float it and leave 2D (class X4) alone.
struct TilingRule { std::string id, name, where, rule; bool tested; };
static std::vector<TilingRule> get_tiling_rules() {
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
static std::string get_tiling_wm() {
    if (const char* h = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); h && *h) return "hyprland";
    if (const char* s = std::getenv("SWAYSOCK"); s && *s) return "sway";
    if (const char* i = std::getenv("I3SOCK"); i && *i) return "i3";
    return {};
}
static void show_tiling_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    Menu menu(terminal);
    menu.title = "X4 VR  ·  Tiling window manager rules";
    menu.keys = "↑↓ move   Enter copy   Esc back";
    menu.label_width = 60;
    const auto wm = get_tiling_wm();
    const auto rules = get_tiling_rules();
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

static void show_uninstall_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    auto chosen = make_removals();
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
        const auto source = find_source_dir();
        items.push_back(info("2. Delete the GitHub clone directory"+(source.empty() ? std::string(" you built the mod from.")
                                                                                  : ": "+source.string())));
        if (const auto wm = get_tiling_wm(); !wm.empty())
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
static void show_checklist_screen(x4vr::tui::Terminal& terminal) {
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
int show_menu() {
    using namespace x4vr::tui;
    setenv("X4VR_NO_STEAMVR_QUERY", "1", 1); // the settings checks: no SteamVR client from here
    Terminal terminal;
    if (!terminal.ok()) { std::cerr << "x4vr: the menu needs a terminal (run it in one, or use the subcommands: x4vr help)\n"; return 1; }
    const auto path = get_settings_file();
    if (!std::filesystem::exists(path)) { // first use: the defaults, as x4vr-run would copy them
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::copy_file(get_data_dir()/"stereo.txt", path, error);
        if (const auto list = find_profiles(); !list.empty() && list[0].built_in) load_profile(list[0]);
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
    const auto board = read_notices();
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
        else if (const auto stats = read_headset_stats())
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
            const auto list = find_profiles();
            const auto name = read_current_profile();
            std::vector<std::string> names;
            int at = -1;
            for (size_t i = 0; i < list.size(); ++i) {
                const bool current = list[i].name == name;
                if (current) at = int(i);
                names.push_back(list[i].name+(list[i].built_in ? " (built in)" : "")+(current && !is_profile_applied(list[i]) ? " · changed" : ""));
            }
            if (at < 0) { names.insert(names.begin(), "(none)"); at = 0; }
            items.push_back(choice("profile", "Profile", names, at,
                "←→ loads another set of the settings below. Built in: Steam Frame. Change settings, then \"Save as profile\" to keep them.", live));
            items.push_back(action("profile_save", "Save as profile", "Saves the settings below under a name of your choice ("+get_user_profiles_dir().string()+").", {}));
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
        if (!std::filesystem::exists(get_desktop_file())) items.push_back(action("desktop", "Add to the app launcher", "\"X4 VR\" in your desktop's app menu, rofi or wofi: "+get_desktop_file().string()));
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
                const auto list = find_profiles();
                const size_t index = size_t(item.choice)-(read_current_profile().empty() || std::none_of(list.begin(), list.end(), [](const auto& p) { return p.name == read_current_profile(); }) ? 1 : 0);
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
            auto name = read_current_profile();
            for (const auto& p : find_profiles()) if (p.built_in && p.name == name) name.clear();
            if (const auto typed = menu.prompt(items, "Profile name", name)) menu.messages = {save_profile(*typed)};
        } else if (e.id == "profile_delete") {
            const std::string name = read_current_profile();
            delete_profile(name);
            menu.messages = {"Profile \""+name+"\" deleted; your settings stay as they are."};
        } else if (e.id == "option_copy") {
            const auto how = copy_to_clipboard(make_launch_option());
            menu.messages = {"Copied (" + how + "): " + make_launch_option(), get_launch_option_steps()};
        } else if (e.id == "desktop") menu.messages = {install_desktop_entry()};
        else if (e.id == "report") {
            const Report report = make_report();
            if (report.file.empty()) menu.messages = {"Couldn't write the report: "+report.error};
            else menu.messages = {"Report: "+report.file, "Attach it to a GitHub issue: "+std::string(issues_url)};
        } else if (e.id == "uninstall") show_uninstall_screen(terminal);
        else if (e.id == "checklist") show_checklist_screen(terminal);
        else if (e.id == "tiling") show_tiling_screen(terminal);
    }
}

} // namespace x4vr::linux_launcher
