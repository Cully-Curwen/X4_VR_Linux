// The launcher's menu: x4vr with no command. One screen in sections (status, play, notices, VR
// settings, X4's settings for VR, setup), and the screens it opens: the in-game settings checklist,
// tiling window manager rules, uninstall.

#include "ui/menu.hpp"
#include "../launcher/launcher_settings.hpp"
#include "build_info.h"
#include "common.hpp"
#include "hud.hpp"
#include "install.hpp"
#include "launch.hpp"
#include "report.hpp"
#include "settings_control.hpp"
#include "status.hpp"
#include "ui/profiles.hpp"
#include "ui/terminal_ui.hpp"
#include "vr_settings.hpp"
#include "x4_settings.hpp"
#include "x4_settings_rules.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace x4vr::linux_launcher {

// A notice for the menu's Notices section: from share/x4vr/notices.txt.
struct Notice {
    x4vr::tui::NoticeKind kind;
    std::string text;
};

// A notices.txt line's kind, from how it starts ("tip:", "issue:", "help:"); none if it's no notice.
static std::optional<x4vr::tui::NoticeKind> find_notice_kind(const std::string& line) {
    if (line.starts_with("tip:")) return x4vr::tui::NoticeKind::tip;
    if (line.starts_with("issue:")) return x4vr::tui::NoticeKind::issue;
    if (line.starts_with("help:")) return x4vr::tui::NoticeKind::help;
    return std::nullopt;
}

// Notices: known issues, tips and requests for help, shipped in share/x4vr/notices.txt.
// One per line, "issue: text", "tip: text" or "help: text"; # starts a comment.
static std::vector<Notice> read_notices() {
    std::vector<Notice> list;
    std::ifstream in(get_data_dir()/"notices.txt");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const std::optional<x4vr::tui::NoticeKind> kind = find_notice_kind(line);
        if (!kind) continue;
        std::string text = line.substr(line.find(':')+1);
        text.erase(0, text.find_first_not_of(' '));
        list.push_back({*kind, text});
    }
    return list;
}

// Tiling window managers: rules that float the VR window.
// X4 renders at its window's size, and a tiling window manager resizes the window (and honours
// X4's fullscreen request, which shrinks it to the monitor). In VR the window's class is X4VR
// (x4vr-run sets SDL_APP_ID), so a rule can float it and leave 2D (class X4) alone.
struct TilingRule {
    const char* id;
    const char* name;
    const char* where;
    const char* rule;
    bool tested;
};

static const TilingRule tiling_rules[] = {
    {"hyprland_lua", "Hyprland 0.55+ (Lua config)", "~/.config/hypr/hyprland.lua",
     "hl.window_rule({ name = \"x4-vr\", match = { class = \"^(X4VR)$\" }, float = true, "
     "suppress_event = \"fullscreen maximize\", fullscreen_state = \"0 0\", confine_pointer = true })", true},
    {"hyprland_conf", "Hyprland (hyprland.conf)", "~/.config/hypr/hyprland.conf",
     "windowrule = float, class:^(X4VR)$\nwindowrule = suppressevent fullscreen maximize, class:^(X4VR)$", false},
    {"sway", "Sway", "~/.config/sway/config", "for_window [class=\"X4VR\"] floating enable, fullscreen disable", false},
    {"i3", "i3", "~/.config/i3/config", "for_window [class=\"X4VR\"] floating enable, fullscreen disable", false},
};

static void show_tiling_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    Menu menu(terminal);
    menu.title = "X4 VR  ·  Tiling window manager rules";
    menu.keys = "↑↓ move   Enter copy   Esc back";
    menu.label_width = 60;
    for (;;) {
        std::vector<Item> items{
            info("X4 renders at its window's size. A tiling window manager resizes it (or makes it fullscreen at the monitor's "
                 "size), so the headset gets a smaller image. In VR the window's class is X4VR (2D keeps X4), so a rule can float "
                 "only the VR window at the size the mod sets. Add the rule for yours to its config; it reloads on save. "
                 "Hyprland's Lua rule also keeps the cursor inside X4's window (confine_pointer), so moving the mouse "
                 "can't move focus away and stop mouse look on foot; keybinds still switch windows."),
        };
        for (const TilingRule& rule : tiling_rules) {
            items.push_back(section(std::string(rule.name)+(rule.tested ? "" : "  (untested)")));
            items.push_back(info(std::string("In ")+rule.where+":"));
            std::istringstream lines(rule.rule);
            for (std::string line; std::getline(lines, line);) items.push_back(info("  "+line));
            items.push_back(action(rule.id, "Copy this rule", "Copies the rule above to the clipboard."));
        }

        items.push_back(section("Check"));
        items.push_back(info("Launch in VR, then hyprctl clients (Hyprland) or swaymsg -t get_tree (Sway): the X4VR window should be "
                             "floating, not fullscreen, at the VR resolution. Remove the rule when uninstalling."));
        items.push_back(action("back", "Back"));

        Event event;
        if (!menu.step(items, event)) continue;
        if (event.kind == Event::Back || event.id == "back") return;
        if (event.kind != Event::Activate) continue;
        for (const TilingRule& rule : tiling_rules)
            if (rule.id == event.id) menu.messages = {"Copied ("+copy_to_clipboard(rule.rule)+"): paste it into "+rule.where+"."};
    }
}

static void show_uninstall_screen(x4vr::tui::Terminal& terminal) {
    using namespace x4vr::tui;
    std::vector<Removal> chosen = make_removals();
    Menu menu(terminal);
    menu.title = "X4 VR  ·  Uninstall";
    menu.keys = "↑↓ move   Space tick   Enter select   Esc back";
    menu.label_width = 48;
    for (;;) {
        std::vector<Item> items{section("Remove what the mod set up")};
        for (const Removal& removal : chosen) items.push_back(toggle(removal.id, removal.label, removal.on, removal.help, {}));
        items.push_back(action("go", "Remove the ticked items", "Your 2D settings and saves stay."));
        items.push_back(action("back", "Back"));

        items.push_back(section("Then, by hand"));
        items.push_back(info("1. In Steam: X4 > Properties > General > Launch options: clear the line."));
        const std::filesystem::path source = find_source_dir();
        items.push_back(info("2. Delete the GitHub clone directory"+(source.empty() ? std::string(" you built the mod from.")
                                                                                  : ": "+source.string())));
        items.push_back(info("3. Remove the X4VR window rule from your window manager's config, if you added one "
                             "(Setup > Tiling window manager rules shows it)."));

        Event event;
        if (!menu.step(items, event)) continue;
        if (event.kind == Event::Back || (event.kind == Event::Activate && event.id == "back")) return;
        if (event.kind == Event::Changed)
            for (Removal& removal : chosen)
                if (removal.id == event.id) removal.on = event.item.on;
        if (event.kind == Event::Activate && event.id == "go") menu.messages = uninstall(chosen).done;
    }
}

// A status row for a setting X4 doesn't have as VR needs it, by how much that matters: a red ✗ if
// it's required, a yellow · if it's optional, a yellow ! for a warning.
static x4vr::tui::Item make_wrong_setting_row(const std::string& label, x4vr::linux_port::SettingSeverity severity, const std::string& text) {
    using x4vr::linux_port::SettingSeverity;
    x4vr::tui::Item row = x4vr::tui::status(label, 2, text);
    switch (severity) {
    case SettingSeverity::required:
        break;
    case SettingSeverity::optional:
        row.state = 1;
        row.mark = "·";
        break;
    case SettingSeverity::warning:
        row.state = 1;
        row.mark = "!";
        break;
    }
    return row;
}

// The in-game settings checklist's row for one setting: `want`, what VR needs, and what X4's settings
// file has, from the check on one of `keys`, else (no file yet, or a setting only the game knows) a
// row to check by hand.
static void add_checklist_row(std::vector<x4vr::tui::Item>& items, const std::vector<x4vr::launcher::Check>& checks, bool have,
                              const char* label, const char* want, std::initializer_list<const char*> keys) {
    using x4vr::linux_port::SettingSeverity;
    for (const x4vr::launcher::Check& check : checks)
        for (const char* key : keys) {
            if (check.fix.empty() || check.fix[0].first != key) continue;

            // Found: what VR wants, and what X4 has when it differs.
            std::string text = want;
            const SettingSeverity severity = x4vr::linux_port::get_setting_severity(check);
            if (check.ok) {
                items.push_back(x4vr::tui::status(label, 0, text));
                return;
            }
            text += "   now: "+check.current;
            if (severity == SettingSeverity::optional) text += " (optional)";
            if (severity == SettingSeverity::warning) text += ": "+std::string(x4vr::linux_port::frame_limit_reason);
            items.push_back(make_wrong_setting_row(label, severity, text));
            return;
        }

    // Not in X4's settings file: to check by hand in X4.
    std::string text = want;
    if (!have) text += "   (not known yet)";
    items.push_back(x4vr::tui::status(label, 3, text));
    items.back().mark = "-";
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
        // X4's VR settings (none before the first VR launch).
        const std::optional<std::string> xml = read_x4_vr_settings();
        const bool have = xml.has_value();
        std::vector<x4vr::launcher::Check> checks;
        if (have) checks = x4_checks(*xml, SteamVrQuery::skip);

        // The resolution VR needs: the size the mod computed, from the resolution check.
        std::string resolution = "the VR resolution";
        for (const x4vr::launcher::Check& check : checks)
            if (check.fix.size() == 2 && check.fix[0].first == "res_width") resolution = check.fix[0].second+"x"+check.fix[1].second;

        // The rows, by where each setting is in X4's settings.
        std::vector<Item> items;
        const auto row = [&](const char* label, const char* want, std::initializer_list<const char*> keys) { add_checklist_row(items, checks, have, label, want, keys); };
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
        row("Resolution", resolution.c_str(), {"res_width"});
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

        Event event;
        if (!menu.step(items, event)) continue;
        if (event.kind == Event::Back || event.id == "back") return;
    }
}

// The status rows that depend on more than one thing (terminal_ui.hpp's states: 0 ok, 1 warning,
// 2 problem, 3 neutral).
// Steam's launch option for X4 (launch_option.hpp's states: 0 not set, 1 this install's x4vr-run,
// 2 another x4vr-run, 3 something else).
static x4vr::tui::Item make_launch_option_row(const Status& current_status) {
    const char* label = "Steam launch option";
    switch (current_status.option.state) {
    case 1:
        if (current_status.option.value == make_launch_option()) return x4vr::tui::status(label, 0, "set");
        return x4vr::tui::status(label, 0, "set: "+current_status.option.value);
    case 2: return x4vr::tui::status(label, 2, "points to another x4vr-run: copy it again (Setup)");
    case 3: return x4vr::tui::status(label, 2, "set to something else: "+current_status.option.value);
    default:
        if (current_status.option.accounts) return x4vr::tui::status(label, 2, "not set: copy it (Setup)");
        return x4vr::tui::status(label, 2, "start X4 once from Steam first");
    }
}

// X4's settings for VR (status.hpp's settings_to_fix: -1 no config.xml, -2 VR settings not made yet).
static x4vr::tui::Item make_x4_settings_row(const Status& current_status) {
    const char* label = "X4 settings for VR";
    if (current_status.settings_to_fix == -1) return x4vr::tui::status(label, 3, "X4's config.xml not found (start X4 once)");
    if (current_status.settings_to_fix == -2) return x4vr::tui::status(label, 3, "made at the first VR launch");
    if (current_status.to_fix.empty()) return x4vr::tui::status(label, 0, "ready (your 2D settings are kept apart)");

    const int to_look_at = int(current_status.to_fix.size())-current_status.settings_to_fix;
    if (current_status.settings_to_fix == 0) return x4vr::tui::status(label, 1, std::to_string(current_status.to_fix.size())+" to look at (list below)");
    std::string text = std::to_string(current_status.settings_to_fix)+" set at the next VR launch";
    if (to_look_at > 0) text += ", "+std::to_string(to_look_at)+" to look at";
    return x4vr::tui::status(label, 1, text+" (list below)");
}

// X4: not running, running in 2D, or in VR with the headset's frame rate (status.hpp, read_headset_stats).
static x4vr::tui::Item make_x4_row(const Status& current_status) {
    if (!current_status.x4) {
        if (current_status.vr_settings_in_place) return x4vr::tui::status("X4", 3, "not running (VR settings still in place: restored at the next start)");
        return x4vr::tui::status("X4", 3, "not running");
    }
    if (!current_status.vr_settings_in_place) return x4vr::tui::status("X4", 3, "running in 2D");
    const std::optional<x4vr::launcher::Stats> stats = read_headset_stats();
    if (!stats) return x4vr::tui::status("X4 in VR", 1, "running, no frames reaching the headset yet");
    const std::string text = std::to_string(stats->fps)+" fps per eye pair, "+std::to_string(stats->late)+" late, "
                             +std::to_string(stats->repeated)+" repeated (last 2 s)";
    if (stats->late || stats->repeated > 2) return x4vr::tui::status("X4 in VR", 1, text);
    return x4vr::tui::status("X4 in VR", 0, text);
}

// A row of the "X4 settings for VR" list: how much the setting matters, what X4 has now, and for a warning why.
static std::string describe_fix(const Status::Fix& fix) {
    const std::string now = fix.current.empty() ? std::string() : ", now: "+fix.current;
    switch (fix.severity) {
    case x4vr::linux_port::SettingSeverity::required: return "required"+now;
    case x4vr::linux_port::SettingSeverity::optional: return "optional"+now;
    case x4vr::linux_port::SettingSeverity::warning: return "warning"+now+": "+std::string(x4vr::linux_port::frame_limit_reason);
    }
    return now;
}

// The "Flat screen" choices in the menu's order, with the mod's theater value for each (stereo.txt).
struct FlatScreenChoice {
    const char* name;
    int theater;
};

static const FlatScreenChoice flat_screen_choices[] = {{"automatic", 1}, {"always", 2}, {"never", 0}};

// The settings rows' tags: when a change takes effect. Live: in stereo.txt, re-read by the mod every
// half second; next launch: applied by x4vr-run when X4 next starts in VR (resolution, HUD extension).
static const char* const live_tag = "live";
static const char* const next_launch_tag = "next launch";

// The menu: one screen in sections. Status and the X4 settings list are rebuilt every 2 s.
int show_menu() {
    using namespace x4vr::tui;
    Terminal terminal;
    if (!terminal.ok()) { std::cerr << "x4vr: the menu needs a terminal (run it in one, or use the subcommands: x4vr help)\n"; return 1; }

    // TODO: the player's files belong to setup: x4vr setup and x4vr-run (once it is C++)
    // call prepare_user_files(), and x4vr-run.in's own copy of the defaults goes; not the menu's job.
    const std::string prepare_error = prepare_user_files();
    const std::filesystem::path path = get_settings_file();

    Menu menu(terminal);
    menu.title = "X4 VR for Linux";
    menu.keys = "↑↓ move   Enter select   Space tick   ←→ change";
    if (!prepare_error.empty()) menu.messages = {prepare_error};
    menu.timeout_ms = 2000; // live status (SteamVR, X4, headset)
    menu.show({info("Checking X4...")}); // while X4's program is scanned, a second or two
    const X4Scan scan = scan_x4({});

    const std::vector<Notice> board = read_notices();
    bool custom_resolution = false; // "Custom" picked: width and height shown even if they match a listed size
    for (;;) {
        const Status current_status = read_status();
        const VrSettings settings = read_vr_settings();
        // TODO: make_scan_row(scan) returning the whole row (as make_launch_option_row), and a StatusState
        // enum for the 0-3 colours, instead of the text and an out-parameter.
        int scan_state = 3;
        const std::string scan_text = make_scan_summary(scan, scan_state);
        const double wanted_hud_factor = read_hud_factor();
        const double hud_factor = wanted_hud_factor >= 0 ? wanted_hud_factor : current_status.hud;
        std::vector<Item> items;

        items.push_back(info("Version "+std::string(X4VR_BUILD))); // build_info.h: git describe
        items.push_back(section("Status"));
        items.push_back(status("SteamVR", current_status.steamvr ? 0 : 3, current_status.steamvr ? "running" : "not running (Launch starts it)"));
        items.push_back(status("X4 build", scan_state, scan_text));
        // TODO: with make_scan_row: the GPU row the same way, from a GPU result in status.cpp (the report
        // keeps its text).
        int gpu_state = 3;
        const std::string gpu_text = read_gpu_summary(gpu_state);
        items.push_back(status("GPU", gpu_state, gpu_text));
        items.push_back(make_launch_option_row(current_status));
        items.push_back(make_x4_settings_row(current_status));
        if (current_status.hud_off_in_x4) items.push_back(status("HUD extension", 1, "turned off in X4's Extensions menu: turn on \"X4 VR HUD distance\""));
        items.push_back(make_x4_row(current_status));

        items.push_back(section("Play"));
        if (!current_status.x4) items.push_back(action("launch", "Launch X4 in VR",
            "Starts SteamVR if needed, then X4 through Steam with the mod. Steam's own Play button starts the normal 2D game."));
        else if (current_status.vr_settings_in_place) {
            items.push_back(action("recenter", "Recentre the view  (Ctrl+F12)", "Look straight ahead first. Same as Ctrl+F12 in X4, or SteamVR's recentre.", live_tag));
            items.push_back(action("flat", "Flat screen on / automatic  (Ctrl+F11)", "Same as Ctrl+F11 in X4: the game on a flat screen in front of you.", live_tag));
        } else items.push_back(info("X4 is running in 2D: quit it to launch in VR."));

        if (!board.empty()) {
            items.push_back(section("Notices"));
            for (const Notice& entry : board) items.push_back(notice(entry.kind, entry.text));
        }

        items.push_back(section("Settings"));
        items.push_back(info("live: applies at once, also while X4 runs.   next launch: applied when X4 next starts in VR."));
        {
            const std::vector<Profile> list = find_profiles();
            const std::string name = read_current_profile();
            std::vector<std::string> names;
            int at = -1;
            for (size_t i = 0; i < list.size(); ++i) {
                const bool current = list[i].name == name;
                if (current) at = int(i);
                names.push_back(list[i].name+(list[i].built_in ? " (built in)" : "")+(current && !is_profile_applied(list[i]) ? " · changed" : ""));
            }
            if (at < 0) { names.insert(names.begin(), "(none)"); at = 0; }
            items.push_back(choice("profile", "Profile", names, at,
                "←→ loads another set of the settings below. Built in: Steam Frame. Change settings, then \"Save as profile\" to keep them.", live_tag));
            items.push_back(action("profile_save", "Save as profile", "Saves the settings below under a name of your choice ("+get_user_profiles_dir().string()+").", {}));
            if (at < int(list.size()) && name == list[size_t(at)].name && !list[size_t(at)].built_in)
                items.push_back(action("profile_delete", "Delete profile \""+name+"\"", "Your settings stay as they are.", {}));
        }
        items.push_back(status("VR runtime", 3, "SteamVR (OpenVR)"));
        items.push_back(toggle("stereo", "3D (stereo)", settings.stereo,
            "Each eye gets its own image. Off: both eyes see the same flat image (for comparing, or if 3D feels wrong).", live_tag));
        items.push_back(toggle("shared_pose", "Shared pose per eye pair", settings.shared_pose,
            "Both eyes of a pair are submitted with one head pose. Keep on for the Steam Frame: without it the right eye ghosts.", live_tag));
        items.push_back(number("ipd_scale", "World scale", settings.ipd_scale, 0.05, 0.3, 3, 2, "",
            "The distance between your eyes in the game. Below 1 the world looks bigger, above 1 smaller. 1 is real size.", live_tag));
        items.push_back(number("predict", "Head prediction", settings.predict*1000, 1, 0, 100, 0, "ms",
            "How far ahead the head pose is predicted for X4. Higher: less lag when turning your head, but more wobble.", live_tag));
        items.push_back(toggle("async_submit", "Stutter protection", settings.async_submit,
            "Sends an image to SteamVR every headset frame; a late game frame repeats the last image instead of a flash. Keep it on: "
            "off is for troubleshooting only, and loses the flat screen (menus) and the Steam Frame's shared pose.", next_launch_tag));
        items.push_back(toggle("cursor", "Mouse cursor in VR", settings.cursor, "Draws the mouse cursor in the headset.", live_tag));
        {
            std::vector<std::string> names;
            int at = 0; // automatic, unless the file has another choice's value
            for (const FlatScreenChoice& flat_choice : flat_screen_choices) {
                if (flat_choice.theater == settings.theater) at = int(names.size());
                names.push_back(flat_choice.name);
            }
            items.push_back(choice("theater", "Flat screen", names, at,
                "Automatic: menus, the map and views without ship controls go to a flat screen in front of you (Ctrl+F11 switches).", live_tag));
        }
        items.push_back(number("theater_distance", "Flat screen distance", settings.theater_distance, 0.25, 0.5, 10, 2, "m",
            "How far in front of you the flat screen stands.", live_tag));
        items.push_back(number("theater_width", "Flat screen width", settings.theater_width, 0.1, 0.5, 10, 2, "m",
            "The flat screen's width; its height follows X4's image.", live_tag));
        items.push_back(toggle("hud_on", "HUD Scaled", hud_factor > 0,
            "X4's cockpit HUD sits a hand's width from your face. On: an extension moves it back at the same apparent size, in VR "
            "only. X4 counts as modified then (saves flagged); turn Protected UI Mode off in X4's Extension Settings.", next_launch_tag));
        if (hud_factor > 0)
            items.push_back(number("hud_factor", "HUD Scale Ratio", hud_factor, 0.1, 1, 6, 1, "",
                "How many times farther the HUD is, from 1.0 to 6.0 (2.5 is a good start). Enter to type a value.", next_launch_tag));
        {
            const int width = settings.x4_width, height = settings.x4_height;
            std::vector<std::string> names{"automatic"};
            int at = 0;
            for (const Resolution& resolution : vr_resolutions) {
                names.push_back(std::to_string(resolution.width)+"x"+std::to_string(resolution.height));
                if (resolution.width == width && resolution.height == height) at = int(names.size())-1;
            }
            if (width <= 0 || height <= 0) custom_resolution = false;         // automatic, e.g. after loading a profile
            else if (at == 0) custom_resolution = true;               // a size not in the list
            names.push_back(custom_resolution ? "Custom: "+std::to_string(width)+"x"+std::to_string(height) : "Custom");
            if (custom_resolution) at = int(names.size())-1;
            items.push_back(choice("resolution", "X4 resolution in VR", names, at,
                "Automatic: the smallest 16:9 size that covers what SteamVR renders (no wasted pixels). Lower is faster, higher "
                "sharper. Custom: any width and height; the image keeps X4's vertical view and any aspect works.", next_launch_tag));
            if (custom_resolution) {
                items.push_back(number("x4_width", "Resolution Width", width, 16, 640, 16384, 0, "px",
                    "X4's image width in VR. Enter to type it.", next_launch_tag));
                items.push_back(number("x4_height", "Resolution Height", height, 16, 360, 16384, 0, "px",
                    "X4's image height in VR. Enter to type it.", next_launch_tag));
            }
        }

        items.push_back(section("X4 settings for VR"));
        items.push_back(action("checklist", "In-game settings checklist",
            "Every X4 setting VR needs, by where it is in X4's settings, with what X4 has now. For troubleshooting."));
        if (!current_status.to_fix.empty()) {
            items.push_back(info("X4 VR uses its own copy of X4's settings: use the checklist to check your settings, required settings will automatically be set for you at launch."));
            for (const Status::Fix& fix : current_status.to_fix) items.push_back(make_wrong_setting_row(fix.label, fix.severity, describe_fix(fix)));
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

        Event event;
        if (!menu.step(items, event)) continue;
        if (event.kind == Event::Back) continue; // Esc doesn't close the menu: only Quit does
        if (event.id == "quit") return 0;

        if (event.kind == Event::Changed) {
            const Item& item = event.item;
            if (event.id == "profile") {
                if (const std::optional<Profile> profile = find_profile_at(item.choice)) {
                    if (load_profile(*profile)) menu.messages = {"Profile \""+profile->name+"\" loaded."};
                    else menu.messages = {"Can't write "+path.string()};
                }
                continue;
            }

            if (event.id == "resolution") {
                Resolution chosen{0, 0}; // automatic
                custom_resolution = item.choice == int(item.choices.size())-1;
                if (custom_resolution) { // start from the size in use: the choice before, else SteamVR's last
                    chosen = {settings.x4_width, settings.x4_height};
                    if (chosen.width <= 0 || chosen.height <= 0) chosen = read_last_vr_resolution().value_or(Resolution{2560, 1440});
                } else if (item.choice > 0) chosen = vr_resolutions[item.choice-1]; // a listed size: after "automatic"
                const bool ok = write_vr_setting("x4_width", chosen.width) && write_vr_setting("x4_height", chosen.height);
                if (!ok) menu.messages = {"Can't write "+path.string()};
                continue;
            }

            if (event.id == "hud_on") {
                if (!set_hud_scaled(item.on, current_status.hud)) menu.messages = {"Can't write "+path.string()};
                continue;
            }

            double value = item.number; // a number row: as shown
            if (item.kind == Item::Toggle) value = item.on ? 1 : 0;
            else if (event.id == "theater") value = flat_screen_choices[item.choice].theater;
            else if (event.id == "predict") value = item.number/1000; // shown in ms, stored in s
            if (!write_vr_setting(event.id, value)) menu.messages = {"Can't write "+path.string()};
            continue;
        }

        if (event.kind != Event::Activate) continue;
        if (event.id == "launch") {
            std::vector<Item> progress_items{section("Launch X4 in VR"), info("Esc: stop waiting")};
            Menu progress(terminal);
            progress.title = menu.title;
            progress.keys = "Esc stop waiting";
            std::vector<std::string> lines;
            const auto show = [&](const std::string& line) { lines.push_back(line); progress.messages = lines; progress.show(progress_items); };
            const auto stop = [&] { return terminal.key(0) == Escape; };
            play(show, stop); // x4vr-run switches X4 to its VR settings, fixes them and applies the HUD distance
            menu.messages = lines;
        } else if (event.id == "recenter") {
            if (x4vr::linux_port::control_settings(path, "recenter") < 0) menu.messages = {"Can't update "+path.string()};
            else menu.messages = {"Recentred."};
        } else if (event.id == "flat") {
            const int theater = x4vr::linux_port::control_settings(path, "flat"); // the new value: 2 always, 1 automatic
            if (theater < 0) menu.messages = {"Can't update "+path.string()};
            else if (theater == 2) menu.messages = {"Flat screen on."};
            else menu.messages = {"Flat screen automatic."};
        } else if (event.id == "profile_save") {
            if (const std::optional<std::string> typed = menu.prompt(items, "Profile name", get_suggested_profile_name()))
                menu.messages = {save_profile(*typed)};
        } else if (event.id == "profile_delete") {
            const std::string name = read_current_profile();
            delete_profile(name);
            menu.messages = {"Profile \""+name+"\" deleted; your settings stay as they are."};
        } else if (event.id == "option_copy") {
            const std::string how = copy_to_clipboard(make_launch_option());
            menu.messages = {"Copied ("+how+"): "+make_launch_option(), "In Steam: X4 > Properties > General > Launch options, paste it there."};
        } else if (event.id == "desktop") menu.messages = {install_desktop_entry()};
        else if (event.id == "report") {
            const Report report = make_report();
            if (report.file.empty()) menu.messages = {"Couldn't write the report: "+report.error};
            else menu.messages = {"Report: "+report.file, "Attach it to a GitHub issue: "+std::string(issues_url)};
        } else if (event.id == "uninstall") show_uninstall_screen(terminal);
        else if (event.id == "checklist") show_checklist_screen(terminal);
        else if (event.id == "tiling") show_tiling_screen(terminal);
    }
}

} // namespace x4vr::linux_launcher
