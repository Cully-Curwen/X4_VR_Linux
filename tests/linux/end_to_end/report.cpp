// `x4vr report` (the bug report) run as players run it, in a fake home directory: the archive is
// made in the home folder and holds the summary, the mod's logs and settings, and X4's config.xml.
// The summary's settings line follows X4's state (status.cpp, read_status()): no config.xml, VR
// settings not made yet, settings to fix, all fixed, also from the VR copy in 2D. The archive is
// readable only by the player; when it can't be made, the message says why (no tar, tar's error)
// and no half-written archive is left. The summary lists the mod's Vulkan layer manifest in each
// folder the Vulkan loader searches.
// usage: test-report <x4vr>
#include "common.hpp"
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// A program on the caller's PATH, as the shell finds it; empty if there is none.
static std::filesystem::path find_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::istringstream dirs(path ? path : "");
    for (std::string dir; std::getline(dirs, dir, ':');)
        if (!dir.empty() && access((std::filesystem::path(dir)/name).c_str(), X_OK) == 0) return std::filesystem::path(dir)/name;
    return {};
}

// The archive `x4vr report` made in the home folder; empty if there is none.
static std::filesystem::path find_archive(const std::filesystem::path& home) {
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(home))
        if (entry.path().filename().string().rfind("x4vr-report-", 0) == 0) return entry.path();
    return {};
}

// Makes a report and returns its summary.txt; the archive is removed, so the next one is the only one.
static std::string make_summary(const char* x4vr, const std::filesystem::path& home, const std::filesystem::path& tar) {
    run_x4vr(x4vr, home, {"report"});
    const std::filesystem::path archive = find_archive(home);
    if (archive.empty() || tar.empty()) return {};
    const std::string summary = run_program({tar.string(), "xzOf", archive.string(), "./summary.txt"}, {get_path_var()}).output;
    std::filesystem::remove(archive);
    return summary;
}

// The number after "x4 settings to fix: " in a summary.
static int get_settings_to_fix(const std::string& summary) {
    const std::string label = "x4 settings to fix: ";
    const size_t at = summary.find(label);
    return at == std::string::npos ? -99 : std::atoi(summary.c_str() + at + label.size());
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: test-report <x4vr>\n"); return 2; }
    namespace fs = std::filesystem;
    const char* x4vr = argv[1];
    char pattern[] = "/tmp/x4vr-report-test-XXXXXX";
    const fs::path home = mkdtemp(pattern);
    const fs::path tar = find_on_path("tar");
    check(!tar.empty(), "tar on PATH (x4vr needs it to pack the report)");

    // X4 never started: no config.xml.
    check(get_settings_to_fix(make_summary(x4vr, home, tar)) == -1, "no config.xml: settings to fix -1");

    // A player who has run the mod: its log and settings, and X4's config.xml (2D, no VR copy yet).
    // A config.xml as X4 writes it, settings inside <root>. Not covered: an empty `<root/>`, which
    // fix-settings can't add settings to; X4 never writes one, so it only exists without X4 installed.
    const fs::path state = home/".local/state/x4vr";
    fs::create_directories(state);
    std::ofstream(state/"x4vr.log") << "x4vr: the mod's log\n";
    std::ofstream(state/"stereo.txt") << "hud_factor=0.8\n";
    const fs::path x4 = home/".config/EgoSoft/X4/123";
    fs::create_directories(x4);
    std::ofstream(x4/"config.xml") << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root>\n  <gamma>1.00</gamma>\n</root>\n";

    Run run = run_x4vr(x4vr, home, {"report"});
    check(run.exit_code == 0, "exit 0");
    check(contains(run.output, "Report: " + (home/"x4vr-report-").string()), "names the archive in the home folder");
    check(contains(run.output, "Attach it to a GitHub issue: https://github.com/"), "says where to send it");

    // The archive: one, in the home folder, readable only by the player, with what was copied into it.
    const fs::path archive = find_archive(home);
    check(!archive.empty() && archive.extension() == ".gz", "archive made");
    if (!archive.empty())
        check(fs::status(archive).permissions() == (fs::perms::owner_read | fs::perms::owner_write), "archive readable only by the player (0600)");
    if (!archive.empty() && !tar.empty()) {
        const Run list = run_program({tar.string(), "tzf", archive.string()}, {get_path_var()});
        for (const char* file : {"./summary.txt", "./build.txt", "./x4vr.log", "./stereo.txt", "./x4-config.xml"})
            check(contains(list.output, std::string(file) + "\n"), std::string("archive holds ") + file);

        const std::string summary = run_program({tar.string(), "xzOf", archive.string(), "./summary.txt"}, {get_path_var()}).output;
        check(contains(summary, "x4vr report "), "summary written");
        check(contains(summary, "launch option check: not set\n"), "summary: launch option not set");
        check(get_settings_to_fix(summary) == -2, "no VR copy yet: settings to fix -2 (made at the first VR launch)");
        fs::remove(archive);
    }

    // A VR session, settings not fixed yet; then fixed; then back in 2D, checked from the VR copy.
    run_x4vr(x4vr, home, {"settings-mode", "vr"});
    check(get_settings_to_fix(make_summary(x4vr, home, tar)) > 0, "VR settings in place, unfixed: some to fix");
    run_x4vr(x4vr, home, {"fix-settings", "--auto"});
    check(get_settings_to_fix(make_summary(x4vr, home, tar)) == 0, "VR settings fixed: none to fix");
    run_x4vr(x4vr, home, {"settings-mode", "2d"});
    check(get_settings_to_fix(make_summary(x4vr, home, tar)) == 0, "back in 2D: the VR copy has none to fix");

    // The mod's layer manifest wherever the Vulkan loader looks for one, each listed once: the XDG
    // config home's default (~/.config), and folders in XDG_CONFIG_DIRS and XDG_DATA_DIRS (NixOS and
    // Flatpak set their own). The data folder is in XDG_DATA_DIRS twice; the loader looks there once.
    const fs::path config_dir = home/"xdg-config", data_dir = home/"xdg-data";
    for (const fs::path& dir : {home/".config", config_dir, data_dir}) {
        fs::create_directories(dir/"vulkan/explicit_layer.d");
        std::ofstream(dir/"vulkan/explicit_layer.d/VkLayer_x4vr.json") << "{}\n";
    }
    run_program({x4vr, "report"}, {"HOME=" + home.string(), get_path_var(), "X4VR_NO_STEAMVR_QUERY=1",
                                   "XDG_CONFIG_DIRS=" + config_dir.string(), "XDG_DATA_DIRS=" + data_dir.string() + ":" + data_dir.string()});
    const fs::path layer_archive = find_archive(home);
    const std::string layer_summary = run_program({tar.string(), "xzOf", layer_archive.string(), "./summary.txt"}, {get_path_var()}).output;
    for (const fs::path& dir : {home/".config", config_dir, data_dir}) {
        const std::string line = "layer manifest: " + (dir/"vulkan/explicit_layer.d/VkLayer_x4vr.json").string() + "\n";
        check(contains(layer_summary, line), "layer manifest listed: " + dir.string());
        check(layer_summary.find(line) == layer_summary.rfind(line), "layer manifest listed once: " + dir.string());
    }
    fs::remove(layer_archive);

    // No tar on PATH: says so, exit 1, nothing made.
    run = run_program({x4vr, "report"}, {"HOME=" + home.string(), "PATH=" + (home/"no-programs").string(), "X4VR_NO_STEAMVR_QUERY=1"});
    check(run.exit_code == 1, "no tar: exit 1");
    check(contains(run.output, "Couldn't write the report: tar didn't run (is it installed?)"), "no tar: says so");
    check(find_archive(home).empty(), "no tar: no archive");

    // tar fails (the home folder doesn't exist, so the archive can't be written): tar's own message.
    run = run_x4vr(x4vr, home/"missing", {"report"});
    check(run.exit_code == 1, "tar fails: exit 1");
    check(contains(run.output, "Couldn't write the report: tar") && contains(run.output, "No such file or directory"), "tar fails: tar's own message");
    check(!contains(run.output, "tar: tar"), "tar fails: tar's message not prefixed twice");

    // tar fails halfway (a full disk): the half-written archive is removed, so no broken report,
    // readable by everyone, is left behind. A fake tar writes some of the archive, then fails.
    const fs::path fake = home/"fake-tar";
    fs::create_directories(fake);
    std::ofstream(fake/"tar") << "#!/bin/sh\nprintf 'half an archive' > \"$2\"\necho 'tar: write error: No space left on device' >&2\nexit 2\n";
    fs::permissions(fake/"tar", fs::perms::owner_all);
    run = run_program({x4vr, "report"}, {"HOME=" + home.string(), "PATH=" + fake.string(), "X4VR_NO_STEAMVR_QUERY=1"});
    check(run.exit_code == 1, "tar fails halfway: exit 1");
    check(contains(run.output, "Couldn't write the report: tar: write error: No space left on device"), "tar fails halfway: tar's own message");
    check(find_archive(home).empty(), "tar fails halfway: the half-written archive removed");

    fs::remove_all(home);
    if (failures) return 1;
    std::printf("report: all checks passed\n");
    return 0;
}
