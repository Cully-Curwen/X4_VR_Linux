// `x4vr fix-settings` (tools/linux/x4_settings.cpp, fix_x4_settings) run as players run it, on a
// config.xml in a fake home directory: what it prints, its exit code, and what it leaves on disk.
// SteamVR isn't asked (X4VR_NO_STEAMVR_QUERY) and the resolution is fixed (X4VR_RESOLUTION).
// usage: fix_settings_tests <x4vr>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const std::string& what) { if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; } }

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// What one run printed (stdout and stderr together) and its exit code.
struct Run {
    std::string output;
    int exit_code = -1;
};

// Runs `x4vr fix-settings [--auto]` with only the environment below, so the player's own
// settings and X4 install are never seen.
Run run_fix_settings(const char* x4vr, const std::string& home, bool quiet) {
    int out[2];
    if (pipe(out) != 0) return {};

    const pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        close(out[0]);
        close(out[1]);
        const std::string home_var = "HOME=" + home;
        char* env[] = {const_cast<char*>(home_var.c_str()), const_cast<char*>("PATH=/usr/bin:/bin"),
                       const_cast<char*>("X4VR_NO_STEAMVR_QUERY=1"), const_cast<char*>("X4VR_RESOLUTION=1920x1080"), nullptr};
        char* argv[] = {const_cast<char*>(x4vr), const_cast<char*>("fix-settings"), const_cast<char*>(quiet ? "--auto" : nullptr), nullptr};
        execve(x4vr, argv, env);
        _exit(127);
    }
    close(out[1]);

    Run run;
    char buffer[4096];
    ssize_t n;
    while ((n = read(out[0], buffer, sizeof buffer)) > 0) run.output.append(buffer, size_t(n));
    close(out[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) run.exit_code = WEXITSTATUS(status);
    return run;
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: fix_settings_tests <x4vr>\n"); return 2; }
    namespace fs = std::filesystem;
    const char* x4vr = argv[1];
    char pattern[] = "/tmp/x4vr-fix-settings-test-XXXXXX";
    const fs::path home = mkdtemp(pattern);
    const fs::path dir = home/".config/EgoSoft/X4/123";
    const fs::path config = dir/"config.xml";
    const fs::path backup = dir/"config.xml.x4vr-backup";
    const fs::path temporary = dir/"config.xml.x4vr-tmp";
    const std::string original = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root>\n  <gamma>1.00</gamma>\n</root>\n";

    // X4 never started: no config.xml.
    Run run = run_fix_settings(x4vr, home, false);
    check(run.exit_code == 1, "no config.xml: exit 1");
    check(contains(run.output, "config.xml not found"), "no config.xml: says so");

    // A config.xml with none of the VR settings: the table, the fixes, the backup.
    fs::create_directories(dir);
    std::ofstream(config) << original;
    run = run_fix_settings(x4vr, home, false);
    check(run.exit_code == 0, "normal run: exit 0");
    check(contains(run.output, "FIX  VSync off"), "normal run: the table");
    check(contains(run.output, "x4vr: X4 setting set for VR: VSync off (was not set)\n"), "normal run: reports each fix");
    check(contains(run.output, "original kept as " + backup.string() + "\n"), "normal run: names the backup, no quotes");
    check(read_file(backup) == original, "normal run: the backup is the original");
    check(read_file(config) != original, "normal run: config.xml written");
    check(!fs::exists(temporary), "normal run: no temporary file left");
    const std::string fixed = read_file(config);

    // Run again: nothing left to fix, nothing written.
    run = run_fix_settings(x4vr, home, false);
    check(run.exit_code == 0, "second run: exit 0");
    check(contains(run.output, "ok   VSync off") && contains(run.output, "Nothing to fix.\n"), "second run: all fixed");
    check(read_file(config) == fixed, "second run: config.xml unchanged");

    // Quiet, as x4vr-run calls it: no table, nothing at all when there's nothing to fix.
    run = run_fix_settings(x4vr, home, true);
    check(run.exit_code == 0 && run.output.empty(), "quiet, nothing to fix: prints nothing");

    // Quiet with fixes to make: only what changed, and the backup made before is kept.
    std::ofstream(config) << original;
    std::ofstream(backup) << "the player's first settings\n";
    run = run_fix_settings(x4vr, home, true);
    check(run.exit_code == 0, "quiet: exit 0");
    check(!contains(run.output, "FIX ") && contains(run.output, "set for VR: VSync off"), "quiet: no table, the fixes");
    check(read_file(backup) == "the player's first settings\n", "quiet: the backup is never overwritten");

    // The backup can't be made (a link into a folder that doesn't exist): nothing changes.
    std::ofstream(config) << original;
    fs::remove(backup);
    fs::create_symlink("/nonexistent/x4vr-backup", backup);
    run = run_fix_settings(x4vr, home, false);
    check(run.exit_code == 1, "no backup: exit 1");
    check(contains(run.output, "Can't back up " + config.string()), "no backup: says so");
    check(read_file(config) == original, "no backup: config.xml untouched");
    fs::remove(backup);

    // config.xml can't be written (a folder where the temporary file goes): nothing changes.
    fs::create_directory(temporary);
    run = run_fix_settings(x4vr, home, false);
    check(run.exit_code == 1, "can't write: exit 1");
    check(contains(run.output, "Can't write " + config.string()), "can't write: says so");
    check(read_file(config) == original, "can't write: config.xml untouched");

    fs::remove_all(home);
    if (failures) return 1;
    std::printf("fix-settings: all checks passed\n");
    return 0;
}
