// x4vr: the Linux port's command-line tool. Subcommands so far are the Phase 0 measurements of
// docs/LINUX_PORT_PLAN.md; settings, HUD and report commands join them later.
#include "elf_classes.hpp"
#include "opentrack.hpp"
#include <x4vr/session.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr double pi = 3.14159265358979323846;

void usage() {
    std::cout <<
        "Usage: x4vr <command> [options]\n"
        "\n"
        "  vr-check [--seconds N]\n"
        "      Connects to SteamVR (OpenVR), prints the headset, render size and eye positions,\n"
        "      then the head pose 4 times a second for N seconds (default 10). Start SteamVR first.\n"
        "\n"
        "  udp-send [--host 127.0.0.1] [--port 4242] [--rate 90]\n"
        "      Sends OpenTrack UDP head poses (as X4 expects with OpenTrack Support on) and reads\n"
        "      commands from the terminal. Type 'help' once it runs.\n"
        "\n"
        "  elf-classes <executable> <filter>\n"
        "      Lists C++ classes whose type name contains <filter> (e.g. Track), with their base\n"
        "      classes and vtables, from the RTTI of a non-PIE executable such as Linux X4.\n";
}

template<class T> bool parse_number(std::string_view text, T& value) {
    const auto end = text.data()+text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc() && ptr == end;
}

// ---- vr-check -------------------------------------------------------------------------------
void print_pose(const char* label, const x4vr::Matrix& m) {
    // OpenVR seated: +X right, +Y up, -Z forward; R = Ry(yaw) * Rx(pitch) * Rz(roll).
    const double yaw = std::atan2(m.m[0][2], m.m[2][2])*180/pi;
    const double pitch = std::asin(std::fmax(-1.0, std::fmin(1.0, -double(m.m[1][2]))))*180/pi;
    const double roll = std::atan2(m.m[1][0], m.m[1][1])*180/pi;
    std::printf("%s position_m=(%+.3f, %+.3f, %+.3f) yaw=%+7.2f pitch=%+7.2f roll=%+7.2f\n",
                label, m.m[0][3], m.m[1][3], m.m[2][3], yaw, pitch, roll);
}
int vr_check(const std::vector<std::string_view>& args) {
    int seconds = 10;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i+1 < args.size() && parse_number(args[i+1], seconds) && seconds >= 0) ++i;
        else { usage(); return 2; }
    }
    std::cout << "runtime_installed=" << vr::VR_IsRuntimeInstalled() << '\n';
    char path[4096]{};
    uint32_t needed = 0;
    if (vr::VR_GetRuntimePath(path, sizeof(path), &needed)) std::cout << "runtime_path=" << path << '\n';
    // Only informative: on Linux this quick check can report no headset while SteamVR has one
    // (streamed headsets, or SteamVR's client library failing to load here). Connecting tells why.
    std::cout << "headset_present=" << vr::VR_IsHmdPresent() << '\n' << std::flush;
    x4vr::Session session;
    try {
        session.initialize();
    } catch (const std::exception& error) {
        std::cerr << "Connecting to SteamVR failed: " << error.what() << "\n"
                     "Is SteamVR running with the headset? If it is, try again through steam-run (docs/LINUX_PHASE0.md, step 3).\n";
        return 4;
    }
    std::cout << "headset_model=" << session.headset_model() << '\n';
    std::cout << "required_instance_extensions=";
    for (const auto& ext : session.instance_extensions()) std::cout << ext << ' ';
    std::cout << '\n';
    x4vr::Frame frame;
    auto status = x4vr::FrameStatus::tracking_unavailable;
    for (auto deadline = std::chrono::steady_clock::now()+5s; std::chrono::steady_clock::now() < deadline;) {
        status = session.begin_frame(frame, 0.05f, 10000.f);
        if (status == x4vr::FrameStatus::ready || status == x4vr::FrameStatus::quit_requested) break;
        std::this_thread::sleep_for(20ms);
    }
    if (status != x4vr::FrameStatus::ready) {
        std::cerr << "frame_unavailable=" << int(status) << ' ' << session.last_error() << '\n';
        return 5;
    }
    std::cout << "per_eye_render_size=" << frame.width << 'x' << frame.height << '\n';
    for (int e = 0; e < 2; ++e) print_pose(e ? "right_eye" : "left_eye ", frame.eyes[e].seated_from_eye);
    std::cout << "Head pose for " << seconds << " s (seated space; move your head):\n" << std::flush;
    int good = 0, bad = 0;
    for (auto end = std::chrono::steady_clock::now()+std::chrono::seconds(seconds); std::chrono::steady_clock::now() < end;) {
        x4vr::Matrix head;
        status = session.sample_tracking(head);
        if (status == x4vr::FrameStatus::quit_requested) break;
        if (status == x4vr::FrameStatus::ready) { ++good; print_pose("head", head); }
        else { ++bad; std::puts("head tracking_unavailable"); }
        std::fflush(stdout);
        std::this_thread::sleep_for(250ms);
    }
    session.shutdown();
    std::cout << "samples_tracked=" << good << " samples_untracked=" << bad << '\n';
    return good > 0 || seconds == 0 ? 0 : 5;
}

// ---- udp-send -------------------------------------------------------------------------------
struct Sender {
    std::mutex mutex;
    x4vr::opentrack::Pose base;
    int alt_axis = -1; double alt_value = 0;               // alternates +value/-value per packet
    int sweep_axis = -1; double sweep_amplitude = 0, sweep_period = 4; // sine on one axis
    bool paused = false;
    std::atomic<uint64_t> sent{0}, failed{0};
};
std::string describe(Sender& s) {
    std::lock_guard lock(s.mutex);
    std::ostringstream out;
    out << "pose:";
    for (int i = 0; i < 6; ++i) out << ' ' << x4vr::opentrack::axis_names[i] << '=' << s.base.axis(i);
    if (s.alt_axis >= 0) out << " | alt " << x4vr::opentrack::axis_names[s.alt_axis] << " +-" << s.alt_value;
    if (s.sweep_axis >= 0) out << " | sweep " << x4vr::opentrack::axis_names[s.sweep_axis] << " +-" << s.sweep_amplitude << " every " << s.sweep_period << " s";
    out << (s.paused ? " | PAUSED" : "") << " | sent " << s.sent << " failed " << s.failed;
    return out.str();
}
void udp_help() {
    std::cout <<
        "Commands (OpenTrack units: x y z in cm, yaw pitch roll in degrees):\n"
        "  <axis> <value>                set one axis, e.g. 'yaw 30' or 'z -5'\n"
        "  zero                          all axes to 0, alternation and sweep off\n"
        "  alt <axis> <value> | alt off  alternate +value/-value every packet (smoothing test)\n"
        "  sweep <axis> <amplitude> <period_s> | sweep off\n"
        "  pause | resume                stop/start sending (what does X4 do without packets?)\n"
        "  show | help | quit\n";
}
int udp_send(const std::vector<std::string_view>& args) {
    std::string host = "127.0.0.1";
    int port = x4vr::opentrack::default_port;
    double rate = 90;
    for (size_t i = 0; i < args.size(); i += 2) {
        const auto key = args[i];
        const auto value = i+1 < args.size() ? args[i+1] : std::string_view();
        if (key == "--host" && !value.empty()) host = std::string(value);
        else if (key == "--port" && parse_number(value, port) && port > 0 && port < 65536) {}
        else if (key == "--rate" && parse_number(value, rate) && rate >= 1 && rate <= 1000) {}
        else { usage(); return 2; }
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) { std::cerr << "Not an IPv4 address: " << host << '\n'; return 2; }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    Sender sender;
    std::atomic_bool running{true};
    std::thread thread([&] {
        const auto start = std::chrono::steady_clock::now();
        const auto period = std::chrono::duration<double>(1.0/rate);
        auto next = start;
        for (uint64_t n = 0; running; ++n) {
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            x4vr::opentrack::Pose pose;
            bool paused;
            {
                std::lock_guard lock(sender.mutex);
                pose = sender.base;
                paused = sender.paused;
                if (sender.alt_axis >= 0) pose.axis(sender.alt_axis) += (n % 2 ? -1 : 1)*sender.alt_value;
                if (sender.sweep_axis >= 0) {
                    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                    pose.axis(sender.sweep_axis) += sender.sweep_amplitude*std::sin(2*pi*t/sender.sweep_period);
                }
            }
            if (!paused) {
                const auto packet = x4vr::opentrack::encode(pose);
                if (sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == ssize_t(packet.size())) ++sender.sent;
                else ++sender.failed;
            }
            std::this_thread::sleep_until(next);
        }
    });
    std::cout << "Sending to " << host << ':' << port << " at " << rate << " Hz.\n";
    udp_help();
    std::cout << describe(sender) << "\n> " << std::flush;
    for (std::string line; std::getline(std::cin, line); std::cout << "> " << std::flush) {
        std::istringstream in(line);
        std::string command;
        if (!(in >> command)) { std::cout << describe(sender) << '\n'; continue; }
        if (command == "quit" || command == "exit") break;
        if (command == "help") { udp_help(); continue; }
        bool ok = true;
        {
            std::lock_guard lock(sender.mutex);
            std::string axis_name; double value = 0, period = 0;
            if (command == "zero") { sender.base = {}; sender.alt_axis = sender.sweep_axis = -1; }
            else if (command == "pause") sender.paused = true;
            else if (command == "resume") sender.paused = false;
            else if (command == "show") {}
            else if (command == "alt" || command == "sweep") {
                const bool sweep = command == "sweep";
                in >> axis_name;
                if (axis_name == "off") (sweep ? sender.sweep_axis : sender.alt_axis) = -1;
                else if (const int axis = x4vr::opentrack::axis_index(axis_name); axis >= 0 && (in >> value) && (!sweep || ((in >> period) && period > 0))) {
                    if (sweep) { sender.sweep_axis = axis; sender.sweep_amplitude = value; sender.sweep_period = period; }
                    else { sender.alt_axis = axis; sender.alt_value = value; }
                } else ok = false;
            } else if (const int axis = x4vr::opentrack::axis_index(command); axis >= 0 && (in >> value)) sender.base.axis(axis) = value;
            else ok = false;
        }
        if (!ok) std::cout << "Didn't understand that. Type 'help'.\n";
        std::cout << describe(sender) << '\n';
    }
    running = false;
    thread.join();
    close(fd);
    std::cout << "\nStopped. " << describe(sender) << '\n';
    return 0;
}

// ---- elf-classes ----------------------------------------------------------------------------
int elf_classes(const std::vector<std::string_view>& args) {
    if (args.size() != 2) { usage(); return 2; }
    const auto image = x4vr::elf::Image::load(std::string(args[0]));
    const auto classes = x4vr::elf::find_classes(image, args[1]);
    for (const auto& c : classes) {
        std::printf("class %s  [%s]  type_info 0x%llx\n", c.name.c_str(), c.mangled.c_str(), static_cast<unsigned long long>(c.typeinfo));
        for (const auto& base : c.bases) std::printf("  base %s\n", base.c_str());
        if (c.vtables.empty()) std::puts("  no vtable found");
        for (const auto& v : c.vtables) {
            std::printf("  vtable address_point 0x%llx  offset_to_top %lld  %zu slots\n",
                        static_cast<unsigned long long>(v.address_point()), static_cast<long long>(v.offset_to_top), v.slots.size());
            for (size_t i = 0; i < v.slots.size(); ++i)
                std::printf("    [%3zu] +0x%03zx  0x%llx\n", i, i*8, static_cast<unsigned long long>(v.slots[i]));
        }
    }
    std::printf("%zu classes matching \"%.*s\"\n", classes.size(), int(args[1].size()), args[1].data());
    return classes.empty() ? 3 : 0;
}
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string_view command = argv[1];
    const std::vector<std::string_view> args(argv+2, argv+argc);
    try {
        if (command == "vr-check") return vr_check(args);
        if (command == "udp-send") return udp_send(args);
        if (command == "elf-classes") return elf_classes(args);
        if (command == "help" || command == "--help" || command == "-h") { usage(); return 0; }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "x4vr " << command << ": " << error.what() << '\n';
        return 1;
    }
}
