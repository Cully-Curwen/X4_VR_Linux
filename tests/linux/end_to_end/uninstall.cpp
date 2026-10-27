// `x4vr uninstall` run as players run it, in a fake home directory with everything the mod sets up:
// alone, it lists what each name removes and removes nothing; an unknown name is a usage error;
// while X4 runs, nothing is removed; each name removes its part and nothing else. The last case
// installs this build as `cmake --install` does (into the fake home) and has the installed x4vr
// remove it: no file of the install may be left, so uninstall's list can't fall behind CMake's.
// usage: test-uninstall <x4vr>
// Run with `--stay`, it stands in for a running X4 (copied as a file named X4) until killed.
#include "common.hpp"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// Runs `x4vr uninstall <names>` in `home`, with X4 installed in `game`.
static Run run_uninstall(const std::string& x4vr, const std::filesystem::path& home, const std::filesystem::path& game,
                         const std::vector<std::string>& names) {
    std::vector<std::string> argv{x4vr, "uninstall"};
    argv.insert(argv.end(), names.begin(), names.end());
    return run_program(argv, {"HOME=" + home.string(), get_path_var(), "X4VR_GAME_DIR=" + game.string()});
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--stay") { pause(); return 0; }
    if (argc != 2) { std::fprintf(stderr, "usage: test-uninstall <x4vr>\n"); return 2; }
    namespace fs = std::filesystem;
    const std::string x4vr = argv[1];
    char pattern[] = "/tmp/x4vr-uninstall-test-XXXXXX";
    const fs::path home = mkdtemp(pattern);

    // What the mod sets up: the desktop entry, the HUD extension (in place, put aside, and in X4's
    // content.xml), X4's settings with the mod's three copies, the mod's state folder (and a file of
    // the player's in it).
    const fs::path desktop = home/".local/share/applications/x4vr.desktop";
    fs::create_directories(desktop.parent_path());
    std::ofstream(desktop) << "[Desktop Entry]\n";
    const fs::path game = home/"X4 Foundations";
    fs::create_directories(game/"extensions/x4vr_hud");
    fs::create_directories(game/"x4vr_hud.off");
    const fs::path x4 = home/".config/EgoSoft/X4/123";
    fs::create_directories(x4);
    const fs::path config = x4/"config.xml", content = x4/"content.xml";
    std::ofstream(config) << "<root>2D, changed since</root>\n";
    std::ofstream(config.string() + ".x4vr-backup") << "<root>2D, before the mod</root>\n";
    std::ofstream(config.string() + ".x4vr-2d") << "<root>2D</root>\n";
    std::ofstream(config.string() + ".x4vr-vr") << "<root>VR</root>\n";
    std::ofstream(content) << "<content>\n  <extension id=\"other\" enabled=\"true\"/>\n  <extension id=\"x4vr_hud\" enabled=\"true\"/>\n</content>\n";
    const fs::path state = home/".local/state/x4vr";
    fs::create_directories(state);
    std::ofstream(state/"stereo.txt") << "hud_factor=0.8\n";
    std::ofstream(state/"x4vr.log") << "x4vr: the mod's log\n";
    std::ofstream(state/"notes.txt") << "the player's own file\n";

    // No names: what each name removes, nothing removed.
    Run run = run_uninstall(x4vr, home, game, {});
    check(run.exit_code == 0, "no names: exit 0");
    for (const char* name : {"desktop", "hud", "restore-x4", "x4-copies", "state", "program"})
        check(contains(run.output, std::string("  ") + name + " "), std::string("no names: lists ") + name);
    check(fs::exists(desktop) && fs::exists(state/"stereo.txt"), "no names: nothing removed");

    // An unknown name: the usage, exit 2, nothing removed (not even the names before it).
    run = run_uninstall(x4vr, home, game, {"desktop", "everything"});
    check(run.exit_code == 2 && contains(run.output, "Usage: x4vr"), "unknown name: usage, exit 2");
    check(fs::exists(desktop), "unknown name: nothing removed");

    // X4 running (it rewrites config.xml as it exits): nothing removed, exit 1.
    const fs::path fake_x4 = home/"X4";
    fs::copy_file("/proc/self/exe", fake_x4);
    const pid_t running = fork();
    if (running == 0) {
        execl(fake_x4.c_str(), "X4", "--stay", nullptr);
        _exit(127);
    }
    const fs::path x4_exe = "/proc/" + std::to_string(running) + "/exe";
    for (int i = 0; i < 1000; ++i) { // until x4vr can see it (it is still the test program right after fork)
        std::error_code error;
        if (fs::read_symlink(x4_exe, error).filename() == "X4") break;
        usleep(1000);
    }
    run = run_uninstall(x4vr, home, game, {"desktop"});
    kill(running, SIGTERM);
    waitpid(running, nullptr, 0);
    check(run.exit_code == 1 && contains(run.output, "X4 is running: close it first"), "X4 running: says so, exit 1");
    check(fs::exists(desktop), "X4 running: nothing removed");

    // desktop: the desktop entry only.
    run = run_uninstall(x4vr, home, game, {"desktop"});
    check(run.exit_code == 0 && contains(run.output, "Desktop entry removed."), "desktop: says so");
    check(!fs::exists(desktop), "desktop: entry removed");
    check(fs::exists(game/"extensions/x4vr_hud") && fs::exists(state/"stereo.txt"), "desktop: nothing else removed");
    check(contains(run.output, "Then, by hand: clear X4's launch option in Steam"), "desktop: says what's left to do by hand");

    // hud: the extension in place and put aside, and its line in X4's content.xml; other extensions stay.
    run = run_uninstall(x4vr, home, game, {"hud"});
    check(run.exit_code == 0 && contains(run.output, "HUD distance extension removed."), "hud: says so");
    check(!fs::exists(game/"extensions/x4vr_hud") && !fs::exists(game/"x4vr_hud.off"), "hud: extension removed, in place and aside");
    check(!contains(read_file(content), "x4vr_hud") && contains(read_file(content), "id=\"other\""), "hud: content.xml line removed, others kept");

    // restore-x4: config.xml back to the backup from before the mod; the copies stay.
    run = run_uninstall(x4vr, home, game, {"restore-x4"});
    check(run.exit_code == 0 && contains(run.output, "X4's settings restored from before the mod."), "restore-x4: says so");
    check(read_file(config) == "<root>2D, before the mod</root>\n", "restore-x4: config.xml from the backup");
    check(fs::exists(config.string() + ".x4vr-backup"), "restore-x4: copies kept");

    // x4-copies: the three copies; config.xml stays.
    run = run_uninstall(x4vr, home, game, {"x4-copies"});
    check(run.exit_code == 0 && contains(run.output, "The mod's copies of X4's settings deleted."), "x4-copies: says so");
    for (const char* suffix : {".x4vr-backup", ".x4vr-2d", ".x4vr-vr"})
        check(!fs::exists(config.string() + suffix), std::string("x4-copies: removed ") + suffix);
    check(fs::exists(config), "x4-copies: config.xml kept");

    // state: the mod's files; the player's own file, and so the folder, stay.
    run = run_uninstall(x4vr, home, game, {"state"});
    check(run.exit_code == 0 && contains(run.output, "Mod settings and logs deleted."), "state: says so");
    check(!fs::exists(state/"stereo.txt") && !fs::exists(state/"x4vr.log"), "state: the mod's files removed");
    check(fs::exists(state/"notes.txt"), "state: the player's file kept");

    // program: this build installed by CMake (its install rules, in the build folder next to x4vr)
    // under the fake home, then removed by the installed x4vr: not one file of the install left.
    const fs::path root = home/"install", cmake = find_on_path("cmake");
    check(!cmake.empty(), "program: cmake on PATH (to install the build)");
    const Run installing = run_program({cmake.string(), "--install", fs::path(x4vr).parent_path().string()}, {"DESTDIR=" + root.string(), get_path_var()});
    check(installing.exit_code == 0, "program: the build installed (build all of it first: the install needs the mod too); cmake --install said:\n" + installing.output);
    std::vector<fs::path> install;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, error))
        if (!entry.is_directory()) install.push_back(entry.path());
    fs::path installed;
    for (const fs::path& file : install)
        if (file.filename() == "x4vr" && file.parent_path().filename() == "bin") installed = file;
    check(!installed.empty(), "program: the installed x4vr found");
    run = run_uninstall(installed.string(), home, game, {"program"});
    check(run.exit_code == 0 && contains(run.output, "Program files removed: "), "program: says so");
    for (const fs::path& file : install) check(!fs::exists(fs::symlink_status(file)), "program: removed " + file.string());
    check(fs::exists(config) && fs::exists(state/"notes.txt"), "program: the player's files kept");

    fs::remove_all(home);
    if (failures) return 1;
    std::printf("uninstall: all checks passed\n");
    return 0;
}
