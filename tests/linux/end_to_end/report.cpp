// `x4vr report` (the bug report) run as players run it, in a fake home directory: the archive is
// made in the home folder and holds the summary, the mod's logs and settings, and X4's config.xml.
// The summary's settings line follows X4's state (status.cpp, read_status()): no config.xml, VR
// settings not made yet, settings to fix, all fixed, also from the VR copy in 2D.
// usage: test-report <x4vr>
#include "run_x4vr.hpp"
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

    const Run run = run_x4vr(x4vr, home, {"report"});
    check(run.exit_code == 0, "exit 0");
    check(contains(run.output, "Report: " + (home/"x4vr-report-").string()), "names the archive in the home folder");
    check(contains(run.output, "Attach it to a GitHub issue: https://github.com/"), "says where to send it");

    // The archive: one, in the home folder, with what was copied into it.
    const fs::path archive = find_archive(home);
    check(!archive.empty() && archive.extension() == ".gz", "archive made");
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

    fs::remove_all(home);
    if (failures) return 1;
    std::printf("report: all checks passed\n");
    return 0;
}
