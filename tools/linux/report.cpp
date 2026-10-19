// The bug report: the mod's logs and settings, X4's config.xml and a summary in
// ~/x4vr-report-<time>.tar.gz. Also what tells a hang at X4's start apart: Steam's, SteamVR's and the GPU driver's versions,
// SteamVR's own logs, and while X4 runs stuck, what each of its threads waits on.

#include "report.hpp"
#include "build_info.h"
#include "code_scan.hpp"
#include "common.hpp"
#include "elf_classes.hpp"
#include "report_info.hpp"
#include "status.hpp"
#include "steam_config.hpp"
#include "x4.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <gnu/libc-version.h>
#include <sstream>
#include <string_view>
#include <sys/utsname.h>
#include <system_error>
#include <unistd.h>
#include <vector>
#include <vulkan/vulkan.h>

namespace x4vr::linux_launcher {

// The first line of a file (/proc and /sys files are one line); empty if it can't be read.
static std::string read_first_line(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

// Whether a program is on PATH, as the shell would find it.
static bool is_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::istringstream dirs(path ? path : "");
    for (std::string dir; std::getline(dirs, dir, ':');)
        if (!dir.empty() && access((std::filesystem::path(dir)/name).c_str(), X_OK) == 0) return true;
    return false;
}

// The end of a log, at most `limit` bytes, from a line start (as the Windows launcher's copy_tail()).
static void copy_tail(const std::filesystem::path& from, const std::filesystem::path& to, std::uintmax_t limit = 1 << 20) {
    std::ifstream in(from, std::ios::binary);
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(from, error);
    if (!in || error) return;
    if (size > limit) {
        in.seekg(static_cast<std::streamoff>(size-limit));
        std::string skipped;
        std::getline(in, skipped);
    }
    std::ofstream(to, std::ios::binary) << in.rdbuf();
}

// The kernel's GPUs: vendor, PCI device id, driver (and NVIDIA's driver version).
static std::vector<std::string> read_sysfs_gpus() {
    std::vector<std::string> gpus;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("card") || name.find('-') != std::string::npos) continue; // cardN-HDMI-A-1: an output
        std::error_code link_error;
        const std::string driver = std::filesystem::read_symlink(entry.path()/"device/driver", link_error).filename().string();
        gpus.push_back(name+": "+x4vr::report::gpu_vendor(read_first_line(entry.path()/"device/vendor"))+" "+read_first_line(entry.path()/"device/device")+
                       ", driver "+(driver.empty() ? "?" : driver));
    }
    std::sort(gpus.begin(), gpus.end());
    if (const std::string nvidia = read_first_line("/proc/driver/nvidia/version"); !nvidia.empty()) gpus.push_back("nvidia: "+nvidia);
    return gpus;
}

// The Vulkan loader's version ("1.4.321"), or why there's none.
static std::string find_vulkan_loader_version() {
    void* vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) return "none (libvulkan.so.1 not found)";
    std::string text = "1.0 (no vkEnumerateInstanceVersion)";
    const PFN_vkGetInstanceProcAddr gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan, "vkGetInstanceProcAddr"));
    const PFN_vkEnumerateInstanceVersion version = gipa ? reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion")) : nullptr;
    uint32_t v = 0;
    if (version && version(&v) == VK_SUCCESS)
        text = std::to_string(VK_API_VERSION_MAJOR(v))+"."+std::to_string(VK_API_VERSION_MINOR(v))+"."+std::to_string(VK_API_VERSION_PATCH(v));
    dlclose(vulkan);
    return text;
}

// Vulkan's GPUs: name, driver and its version (e.g. "NVIDIA GeForce RTX 4090, NVIDIA 595.99.02",
// "AMD Radeon RX 7900 XTX (RADV NAVI31), radv Mesa 25.2.3"); not CPU renderers.
static std::vector<std::string> find_vulkan_gpus() {
    std::vector<std::string> gpus;
    void* vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) return gpus;
    const PFN_vkGetInstanceProcAddr gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan, "vkGetInstanceProcAddr"));
    const PFN_vkCreateInstance create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "x4vr"; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create && create(&info, nullptr, &instance) == VK_SUCCESS) {
        const PFN_vkEnumeratePhysicalDevices enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
        const PFN_vkGetPhysicalDeviceProperties2 properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
        uint32_t count = 0;
        if (enumerate && properties2 && enumerate(instance, &count, nullptr) == VK_SUCCESS) {
            std::vector<VkPhysicalDevice> devices(count);
            enumerate(instance, &count, devices.data());
            for (const VkPhysicalDevice device : devices) {
                VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceProperties2 gpu{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                gpu.pNext = &driver;
                properties2(device, &gpu);
                if (gpu.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue; // llvmpipe: not a GPU
                std::string text = gpu.properties.deviceName;
                if (*driver.driverName) text += std::string(", ")+driver.driverName+(*driver.driverInfo ? std::string(" ")+driver.driverInfo : "");
                gpus.push_back(text);
            }
        }
        reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);
    }
    dlclose(vulkan);
    return gpus;
}

// X4's compatibility tool in Steam (report_info.hpp): "none (native)", or e.g. "proton_9: the
// Windows X4 runs, without the Linux mod".
static std::string read_x4_compat_tool() {
    for (const std::filesystem::path& steam : x4vr::steam::steam_roots())
        if (const std::string tool = x4vr::report::x4_compat_tool(read_text(steam/"config/config.vdf")); !tool.empty())
            return tool+": the Windows X4 runs, without the Linux mod";
    return "none (native)";
}

// Each X4 thread: id, name, state and the kernel function it sleeps in (wchan: futex_* is a
// lock, e.g. a deadlock; unix_stream_*, *poll*, sock_* is a wait on another process, e.g. SteamVR).
static std::string read_x4_threads(int pid) {
    std::ostringstream out;
    out << "tid name state wchan\n";
    std::error_code error;
    std::vector<std::filesystem::path> tasks;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator("/proc/"+std::to_string(pid)+"/task", error)) tasks.push_back(entry.path());
    std::sort(tasks.begin(), tasks.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) { return std::atoi(a.filename().c_str()) < std::atoi(b.filename().c_str()); });
    for (const std::filesystem::path& task : tasks) {
        const std::string stat = read_first_line(task/"stat"); // pid (comm) state ...: comm may hold spaces
        const size_t close = stat.rfind(')');
        const char state = close != std::string::npos && close+2 < stat.size() ? stat[close+2] : '?';
        const std::string wchan = read_first_line(task/"wchan");
        out << task.filename().string() << ' ' << read_first_line(task/"comm") << ' ' << state << ' ' << (wchan.empty() || wchan == "0" ? "-" : wchan) << "\n";
    }
    return out.str();
}

// The launch option's state (launch_option.hpp) in words, on its own line: the value above it is
// the player's launch option exactly, to compare or copy.
static std::string describe_launch_option(int state) {
    switch (state) {
    case 0: return "not set";
    case 1: return "OK: runs this install's x4vr-run";
    case 2: return "points to another x4vr-run (an older install?)";
    default: return "doesn't run x4vr-run";
    }
}

std::string make_build_text() {
    std::string text = std::string("x4vr ")+X4VR_BUILD+"\ncommit "+X4VR_COMMIT;
    if (*X4VR_COMMIT_DATE) text += std::string(" (")+X4VR_COMMIT_DATE+")";
    if (*X4VR_COMMIT_SUBJECT) text += std::string("\n")+X4VR_COMMIT_SUBJECT;
    if (*X4VR_CHANGED_FILES) text += std::string("\nlocal changes: ")+X4VR_CHANGED_FILES;
    if (*X4VR_RECENT_COMMITS) text += std::string("\n\nrecent commits:\n")+X4VR_RECENT_COMMITS;
    return text+"\n";
}

Report make_report() {
    // The archive's name: in the home folder, with the time it was made.
    setenv("X4VR_NO_STEAMVR_QUERY", "1", 1); // the settings check: no SteamVR client from here
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const char* home_var = std::getenv("HOME");
    const std::filesystem::path home = home_var ? home_var : ".";
    const std::filesystem::path out = home/("x4vr-report-"+std::string(stamp)+".tar.gz");

    // A new folder only this user can read (mkdtemp: 0700, a name nobody could create first): the
    // logs and X4's config.xml are gathered in it, then packed.
    std::string pattern = (std::filesystem::temp_directory_path()/"x4vr-report-XXXXXX").string();
    if (!mkdtemp(pattern.data())) return {"", "can't make a folder in "+std::filesystem::temp_directory_path().string()+": "+std::strerror(errno)};
    const std::filesystem::path root = pattern, staging = root/"report";
    std::error_code error;
    std::filesystem::create_directory(staging, error);

    // The mod's logs and settings, X4's config.xml, the build: whichever exist (a missing one is skipped).
    for (const char* name : {"x4vr.log", "x4vr.previous.log", "stderr.log", "stderr.previous.log", "stereo.txt", "x4_resolution.txt", "pair_stats.txt"})
        std::filesystem::copy_file(get_state_dir()/name, staging/name, error);
    if (const std::filesystem::path config = x4_config(); !config.empty()) std::filesystem::copy_file(config, staging/"x4-config.xml", error);
    write_text(staging/"build.txt", make_build_text());

    // The system: kernel, distribution, desktop session and compositor, CPU, memory, glibc, Vulkan.
    std::ostringstream summary;
    summary << "x4vr report " << stamp << "\nx4vr build: " << X4VR_BUILD << "\nx4vr commit: " << X4VR_COMMIT << ' ' << X4VR_COMMIT_SUBJECT << "\n";
    struct utsname u{};
    if (uname(&u) == 0) summary << "system: " << u.sysname << ' ' << u.release << ' ' << u.machine << "\n";
    const auto env = [](const char* name) { const char* value = std::getenv(name); return std::string(value && *value ? value : "-"); };
    summary << "distribution: " << x4vr::report::os_name(read_text("/etc/os-release")) << "\n";
    summary << "session: " << env("XDG_SESSION_TYPE") << ", desktop " << env("XDG_CURRENT_DESKTOP") << "\n";
    std::vector<std::string> processes;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator("/proc", error)) processes.push_back(read_first_line(entry.path()/"comm"));
    const std::string compositor = x4vr::report::compositor(processes);
    summary << "compositor: " << (compositor.empty() ? std::string("unknown") : compositor) << "\n";
    summary << "cpu: " << x4vr::report::cpu(read_text("/proc/cpuinfo")) << "\nmemory: " << x4vr::report::memory(read_text("/proc/meminfo"))
            << "\nglibc: " << gnu_get_libc_version() << "\nvulkan loader: " << find_vulkan_loader_version() << "\n";

    // The mod's status, as the menu shows it (status.cpp).
    const Status status = read_status();
    summary << "steam running: " << status.steam << "\nsteamvr running: " << status.steamvr << "\nx4 running: " << status.x4
            << "\nlaunch option: " << (status.option.value.empty() ? std::string("-") : status.option.value)
            << "\nlaunch option check: " << describe_launch_option(status.option.state) << "\nx4 compatibility tool: " << read_x4_compat_tool()
            << "\nhud factor: " << status.hud
            << (status.hud_off_in_x4 ? " (off in X4)" : "") << "\nx4 settings to fix: " << status.settings_to_fix << "\n";

    // The headset and SteamVR's version from SteamVR's own log: asking SteamVR here could hang the
    // report along with it, and reports are made when something is stuck.
    std::string steamvr_manifest;
    for (const std::filesystem::path& library : x4vr::steam::libraries())
        if (steamvr_manifest.empty()) steamvr_manifest = read_text(library/"steamapps"/("appmanifest_"+std::string(x4vr::report::steamvr_app)+".acf"));
    x4vr::report::SteamVrLog steamvr_log;
    for (const std::filesystem::path& steam : x4vr::steam::steam_roots())
        if (steamvr_log.headset.empty()) steamvr_log = x4vr::report::steamvr_log(read_text(steam/"logs/vrserver.txt"));
    summary << "headset: " << (steamvr_log.headset.empty() ? std::string("unknown (no headset in SteamVR's vrserver.txt)") : steamvr_log.headset) << "\n";
    summary << "steamvr: " << (steamvr_log.version.empty() ? std::string() : steamvr_log.version+", ") << x4vr::report::steamvr_version(steamvr_manifest) << "\n";

    // Each Steam install: the client's version and beta branch, and SteamVR's logs of the last two
    // days (logs/vr*.txt), checked for Steam Frame link problems (Error 17 and the like).
    std::filesystem::create_directory(staging/"steamvr", error);
    for (const std::filesystem::path& steam : x4vr::steam::steam_roots()) {
        std::string client;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(steam/"package", error)) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with("steam_client_") && entry.path().extension() == ".manifest") client = read_text(entry.path());
        }
        if (!client.empty()) summary << "steam client (" << steam.string() << "): " << x4vr::report::steam_client_version(client, read_text(steam/"package/beta")) << "\n";
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(steam/"logs", error)) {
            const std::string name = entry.path().filename().string();
            std::error_code time_error;
            const std::filesystem::file_time_type::duration age = std::filesystem::file_time_type::clock::now()-std::filesystem::last_write_time(entry.path(), time_error);
            if (!name.starts_with("vr") || entry.path().extension() != ".txt" || time_error || age >= std::chrono::hours(48)) continue;
            copy_tail(entry.path(), staging/"steamvr"/name);
            for (const std::string& line : x4vr::report::link_problems(read_text(staging/"steamvr"/name)))
                summary << "steamvr link (" << name << "): " << line << "\n";
        }
    }

    // GPUs and drivers, from the kernel and from Vulkan, and vulkaninfo's summary if installed.
    for (const std::string& gpu : read_sysfs_gpus()) summary << (gpu.starts_with("nvidia: ") ? "" : "gpu ") << gpu << "\n";
    for (const std::string& gpu : find_vulkan_gpus()) summary << "vulkan gpu: " << gpu << "\n";
    if (is_on_path("vulkaninfo")) run({"timeout", "20", "vulkaninfo", "--summary"}, {.log = staging/"vulkaninfo.txt"});

    // The mod's Vulkan layer manifests: more than one gives the loader's "duplicate" warnings.
    for (const char* dir : {"/etc/vulkan", "/usr/share/vulkan", "/usr/local/share/vulkan"})
        if (std::filesystem::exists(std::filesystem::path(dir)/"explicit_layer.d/VkLayer_x4vr.json"))
            summary << "layer manifest: " << dir << "/explicit_layer.d/VkLayer_x4vr.json\n";
    if (const std::filesystem::path local = home/".local/share/vulkan/explicit_layer.d/VkLayer_x4vr.json"; std::filesystem::exists(local))
        summary << "layer manifest: " << local.string() << "\n";
    int gpu_state = 3;
    summary << "gpu (last VR session): " << read_gpu_summary(gpu_state) << "\n";

    // Where the last runs stopped: a SteamVR call that never returned. Stuck when X4 still runs.
    bool stuck = false;
    for (const char* name : {"x4vr.log", "x4vr.previous.log"}) {
        const x4vr::report::LastRun last = x4vr::report::last_run(read_text(get_state_dir()/name));
        if (!last.unfinished) continue;
        const bool current = std::string_view(name) == "x4vr.log";
        stuck = stuck || (current && status.x4 && !last.exit);
        std::string ended = "no exit logged";
        if (last.exit) ended = *last.exit;
        else if (current && status.x4) ended = "X4 still running";
        summary << name << ": stopped in a SteamVR call: " << *last.unfinished << " (" << ended << ")\n";
    }

    // While X4 runs, what each of its threads waits on; when stuck, a backtrace of every thread
    // (gdb or eu-stack, if installed and the kernel's ptrace_scope allows it).
    if (const int pid = find_x4_pid()) {
        write_text(staging/"x4-threads.txt", read_x4_threads(pid));
        if (stuck) {
            const std::string scope = read_first_line("/proc/sys/kernel/yama/ptrace_scope");
            summary << "ptrace_scope: " << (scope.empty() ? "-" : scope) << "\n";
            const std::string pid_text = std::to_string(pid);
            const std::filesystem::path trace = staging/"x4-backtrace.txt";
            bool traced = false;
            if (is_on_path("gdb"))
                traced = run({"timeout", "90", "gdb", "-p", pid_text, "-batch", "-nx", "-ex", "set sysroot /proc/"+pid_text+"/root", "-ex", "thread apply all bt"}, {.log = trace});
            if (!traced && is_on_path("eu-stack")) traced = run({"timeout", "60", "eu-stack", "-p", pid_text}, {.log = trace});
            // Only what's installed: x4-threads.txt alone tells a lock from a wait on SteamVR.
            if (traced) summary << "backtrace: x4-backtrace.txt\n";
            else if (is_on_path("gdb") || is_on_path("eu-stack")) summary << "backtrace: failed, see x4-backtrace.txt\n";
            else summary << "backtrace: none (no gdb or eu-stack); see x4-threads.txt\n";
        }
    }

    // The X4 scan (code_scan.hpp): where the mod's code sites are in this X4 build.
    if (const std::filesystem::path game = find_x4_dir(); !game.empty() && std::filesystem::exists(game/"X4")) {
        summary << "x4 scan (" << (game/"X4").string() << "):\n";
        try {
            const x4vr::linux_port::code::X4Sites sites = x4vr::linux_port::code::find_x4_sites(x4vr::elf::Image::load((game/"X4").string()));
            for (const std::string& note : sites.notes) summary << "  " << note << "\n";
        } catch (const std::exception& e) { summary << "  " << e.what() << "\n"; }
    }

    // The summary, then all of it packed, readable only by the player; if tar failed, its own
    // message and no half-written archive. The gathering folder goes either way.
    write_text(staging/"summary.txt", summary.str());
    const bool packed = run({"tar", "czf", out.string(), "-C", staging.string(), "."}, {.log = root/"tar.log"});
    std::string tar_message = read_text(root/"tar.log");
    while (!tar_message.empty() && std::isspace(static_cast<unsigned char>(tar_message.back()))) tar_message.pop_back();
    std::filesystem::remove_all(root, error);
    if (!packed) {
        std::filesystem::remove(out, error); // half-written: useless, and not yet readable only by the player
        return {"", tar_message.empty() ? "tar didn't run (is it installed?)" : tar_message}; // tar's messages start "tar: "
    }
    std::filesystem::permissions(out, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, error);
    return {out.string(), ""};
}

} // namespace x4vr::linux_launcher
