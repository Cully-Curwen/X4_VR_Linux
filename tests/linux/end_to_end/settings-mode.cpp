// `x4vr settings-mode` (tools/linux/x4_settings.cpp, switch_x4_settings) run as x4vr-run runs it,
// on a config.xml in a fake home directory: X4's 2D settings put aside for VR and back, the
// marker, the HUD extension switched on for VR, and the wait for X4 to finish exiting.
// usage: test-settings-mode <x4vr>
// Run with `--exit-soon`, it stands in for X4 exiting (copied as a file named X4).
#include "common.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--exit-soon") { usleep(200 * 1000); return 0; }
    if (argc != 2) { std::fprintf(stderr, "usage: test-settings-mode <x4vr>\n"); return 2; }
    namespace fs = std::filesystem;
    const char* x4vr = argv[1];
    char pattern[] = "/tmp/x4vr-settings-mode-test-XXXXXX";
    const fs::path home = mkdtemp(pattern);
    const fs::path dir = home/".config/EgoSoft/X4/123";
    const fs::path config = dir/"config.xml";
    const fs::path two_d = dir/"config.xml.x4vr-2d";
    const fs::path vr = dir/"config.xml.x4vr-vr";
    const fs::path content = dir/"content.xml";
    const fs::path marker = home/".local/state/x4vr/x4-settings.vr";
    const std::string settings_2d = "<root>2D</root>\n", settings_vr = "<root>VR</root>\n";

    // X4 never started: nothing to switch, and status says 2D.
    Run run = run_x4vr(x4vr, home, {"settings-mode", "vr"});
    check(run.exit_code == 0 && run.output.empty(), "no config.xml: vr does nothing");
    run = run_x4vr(x4vr, home, {"settings-mode", "status"});
    check(run.exit_code == 0 && contains(run.output, "X4 has its 2D settings"), "no config.xml: status 2D");

    // Wrong arguments: the help, exit 2.
    run = run_x4vr(x4vr, home, {"settings-mode", "bogus"});
    check(run.exit_code == 2 && contains(run.output, "Usage:"), "wrong argument: help, exit 2");
    run = run_x4vr(x4vr, home, {"settings-mode"});
    check(run.exit_code == 2, "no argument: exit 2");

    // To VR: the 2D settings saved, the VR copy from last time in place, the marker written, the
    // HUD extension switched on (it is off outside VR).
    fs::create_directories(dir);
    std::ofstream(config) << settings_2d;
    std::ofstream(vr) << settings_vr;
    std::ofstream(content) << "<content>\n  <extension id=\"x4vr_hud\" enabled=\"false\"/>\n</content>\n";
    run = run_x4vr(x4vr, home, {"settings-mode", "vr"});
    check(run.exit_code == 0, "vr: exit 0");
    check(contains(run.output, "x4vr: X4's 2D settings saved (config.xml.x4vr-2d), VR settings in place"), "vr: says so");
    check(read_file(two_d) == settings_2d, "vr: 2D settings saved");
    check(read_file(config) == settings_vr, "vr: VR settings in place");
    check(read_file(marker) == config.string() + "\n", "vr: the marker names config.xml");
    check(contains(read_file(content), "id=\"x4vr_hud\" enabled=\"true\""), "vr: HUD extension switched on");
    run = run_x4vr(x4vr, home, {"settings-mode", "status"});
    check(run.exit_code == 0 && contains(run.output, "X4 has its VR settings (" + config.string() + ")"), "vr: status VR");

    // To VR again (a launch that didn't get to restore 2D): nothing changes.
    run = run_x4vr(x4vr, home, {"settings-mode", "vr"});
    check(run.exit_code == 0 && run.output.empty(), "vr again: does nothing");
    check(read_file(two_d) == settings_2d, "vr again: 2D settings still saved");

    // Back to 2D after X4 changed its VR settings: those kept as the VR copy, 2D back, marker gone.
    const std::string changed_vr = "<root>VR, changed in X4</root>\n";
    std::ofstream(config) << changed_vr;
    run = run_x4vr(x4vr, home, {"settings-mode", "2d"});
    check(run.exit_code == 0, "2d: exit 0");
    check(contains(run.output, "x4vr: X4's VR settings saved (config.xml.x4vr-vr), 2D settings restored"), "2d: says so");
    check(read_file(config) == settings_2d, "2d: 2D settings back");
    check(read_file(vr) == changed_vr, "2d: VR settings kept for next time");
    check(!fs::exists(marker), "2d: marker removed");
    run = run_x4vr(x4vr, home, {"settings-mode", "status"});
    check(run.exit_code == 0 && contains(run.output, "X4 has its 2D settings (" + config.string() + ")"), "2d: status 2D");

    // To 2D again: nothing changes.
    run = run_x4vr(x4vr, home, {"settings-mode", "2d"});
    check(run.exit_code == 0 && run.output.empty(), "2d again: does nothing");
    check(read_file(config) == settings_2d && read_file(vr) == changed_vr, "2d again: files unchanged");

    // X4 still exiting (x4vr-run runs as X4 closes): x4vr waits for it, then switches.
    const fs::path fake_x4 = home/"X4";
    fs::copy_file("/proc/self/exe", fake_x4);
    const pid_t x4 = fork();
    if (x4 == 0) {
        execl(fake_x4.c_str(), "X4", "--exit-soon", nullptr);
        _exit(127);
    }
    const fs::path x4_exe = "/proc/" + std::to_string(x4) + "/exe";
    for (int i = 0; i < 1000; ++i) { // until x4vr can see it (it is still the test program right after fork)
        std::error_code error;
        if (fs::read_symlink(x4_exe, error).filename() == "X4") break;
        usleep(1000);
    }
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    run = run_x4vr(x4vr, home, {"settings-mode", "vr"});
    const std::chrono::steady_clock::duration waited = std::chrono::steady_clock::now() - start;
    waitpid(x4, nullptr, 0);
    check(waited >= std::chrono::milliseconds(50), "X4 exiting: x4vr waited for it");
    check(run.exit_code == 0 && contains(run.output, "VR settings in place"), "X4 exiting: then switched");
    check(read_file(config) == changed_vr && fs::exists(marker), "X4 exiting: VR settings in place");

    fs::remove_all(home);
    if (failures) return 1;
    std::printf("settings-mode: all checks passed\n");
    return 0;
}
