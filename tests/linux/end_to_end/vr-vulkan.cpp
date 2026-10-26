// `x4vr vr-vulkan` run as x4vr-run runs it, with no SteamVR: it asks Steam to start SteamVR (a fake
// `steam` on PATH records the request), then gives up at once (X4VR_HEADSET_WAIT=0) with exit 1 and
// a message, printing nothing for the mod. The path where SteamVR answers needs SteamVR itself.
// usage: test-vr-vulkan <x4vr>
// Started as `steam` (copied into the fake home's bin/), it stands in for Steam: writes its
// arguments to $HOME/steam-called.
#include "common.hpp"
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc > 0 && fs::path(argv[0]).filename() == "steam") {
        const char* home = std::getenv("HOME");
        std::ofstream called(fs::path(home ? home : ".")/"steam-called");
        for (int i = 1; i < argc; ++i) called << argv[i] << '\n';
        return 0;
    }
    if (argc != 2) { std::fprintf(stderr, "usage: test-vr-vulkan <x4vr>\n"); return 2; }
    const char* x4vr = argv[1];
    char pattern[] = "/tmp/x4vr-vr-vulkan-test-XXXXXX";
    const fs::path home = mkdtemp(pattern);
    const fs::path bin = home/"bin";
    fs::create_directories(bin);
    fs::copy_file("/proc/self/exe", bin/"steam");

    // Only the fake steam on PATH; no SteamVR runtime in the fake home.
    const Run run = run_program({x4vr, "vr-vulkan"}, {"HOME=" + home.string(), "PATH=" + bin.string(), "X4VR_HEADSET_WAIT=0"});
    check(run.exit_code == 1, "exit 1");
    check(contains(run.output, "x4vr: SteamVR isn't running: starting it\n"), "says it starts SteamVR");
    check(contains(run.output, "x4vr: SteamVR's Vulkan needs not read: "), "says why it gave up");

    // Steam asked to start SteamVR: detached, so it may finish just after x4vr.
    const fs::path called = home/"steam-called";
    for (int i = 0; i < 200 && read_file(called).empty(); ++i) usleep(10 * 1000);
    check(read_file(called) == "steam://rungameid/250820\n", "Steam asked to start SteamVR (app 250820)");

    fs::remove_all(home);
    if (failures) return 1;
    std::printf("vr-vulkan: all checks passed\n");
    return 0;
}
