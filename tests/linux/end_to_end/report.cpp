// `x4vr report` (the bug report) run as players run it, in a fake home directory: the archive is
// made in the home folder and holds the summary, the mod's logs and settings, and X4's config.xml.
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

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: test-report <x4vr>\n"); return 2; }
    namespace fs = std::filesystem;
    const char* x4vr = argv[1];
    char pattern[] = "/tmp/x4vr-report-test-XXXXXX";
    const fs::path home = mkdtemp(pattern);
    const fs::path tar = find_on_path("tar");
    check(!tar.empty(), "tar on PATH (x4vr needs it to pack the report)");

    // A player who has run the mod: its log and settings, and X4's config.xml.
    const fs::path state = home/".local/state/x4vr";
    fs::create_directories(state);
    std::ofstream(state/"x4vr.log") << "x4vr: the mod's log\n";
    std::ofstream(state/"stereo.txt") << "hud_factor=0.8\n";
    const fs::path x4 = home/".config/EgoSoft/X4/123";
    fs::create_directories(x4);
    std::ofstream(x4/"config.xml") << "<root/>\n";

    Run run = run_x4vr(x4vr, home, {"report"});
    check(run.exit_code == 0, "exit 0");
    check(contains(run.output, "Report: " + (home/"x4vr-report-").string()), "names the archive in the home folder");
    check(contains(run.output, "Attach it to a GitHub issue: https://github.com/"), "says where to send it");

    // The archive: one, in the home folder, with what was copied into it.
    fs::path archive;
    for (const fs::directory_entry& entry : fs::directory_iterator(home))
        if (entry.path().filename().string().rfind("x4vr-report-", 0) == 0) archive = entry.path();
    check(!archive.empty() && archive.extension() == ".gz", "archive made");
    if (!archive.empty() && !tar.empty()) {
        const Run list = run_program({tar.string(), "tzf", archive.string()}, {get_path_var()});
        for (const char* file : {"./summary.txt", "./build.txt", "./x4vr.log", "./stereo.txt", "./x4-config.xml"})
            check(contains(list.output, std::string(file) + "\n"), std::string("archive holds ") + file);

        const Run summary = run_program({tar.string(), "xzOf", archive.string(), "./summary.txt"}, {get_path_var()});
        check(contains(summary.output, "x4vr report ") && contains(summary.output, "x4 settings to fix: "), "summary written");
    }

    fs::remove_all(home);
    if (failures) return 1;
    std::printf("report: all checks passed\n");
    return 0;
}
