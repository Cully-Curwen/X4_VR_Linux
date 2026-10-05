// Diagnostic (X4VR_WATCH_HEAD=1): which X4 code reads the head position, for stage D (backward
// clamp) and on foot (docs/LINUX_FINDINGS.md). X4 9.00's head-tracker bridge is a static object
// (U::HeadTrackerCameraBridge at 0x3e60820, vtable 0x3b1c240) whose position method stores
// (x, y, -z) at +0x10; whatever turns that into the camera's head offset reads it from there.
// Hardware watchpoints (perf_event_open, own process, no root) on those 16 bytes record each
// instruction that touches them, on every thread, for 20 s once X4 applies head tracking; the log
// then lists the code addresses by count (an address is the instruction after the access).
#include "linux_runtime.hpp"
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace x4vr::linux_port {
namespace {
constexpr uintptr_t bridge_object = 0x3e60820, bridge_vtable = 0x3b1c240;
constexpr size_t ring_pages = 8; // data pages per event, plus one header page

struct Watch { int fd{-1}; void* ring{}; size_t size{}; };

int open_watch(pid_t tid, uintptr_t address, std::string& error) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_RW; // x86 has no read-only data breakpoints
    attr.bp_addr = address;
    attr.bp_len = HW_BREAKPOINT_LEN_8;
    attr.sample_period = 1;
    attr.sample_type = PERF_SAMPLE_IP;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    const int fd = int(syscall(SYS_perf_event_open, &attr, tid, -1, -1, PERF_FLAG_FD_CLOEXEC));
    if (fd < 0) error = std::strerror(errno);
    return fd;
}

// Reads the samples a ring holds (PERF_RECORD_SAMPLE: header, then the IP) into `counts`.
void drain(Watch& w, std::map<uint64_t, uint64_t>& counts) {
    auto* meta = static_cast<perf_event_mmap_page*>(w.ring);
    const uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = meta->data_tail;
    const auto* data = static_cast<const unsigned char*>(w.ring)+meta->data_offset;
    const uint64_t mask = meta->data_size-1;
    while (tail < head) {
        perf_event_header header;
        for (size_t i = 0; i < sizeof header; ++i) reinterpret_cast<unsigned char*>(&header)[i] = data[(tail+i) & mask];
        if (header.type == PERF_RECORD_SAMPLE && header.size >= sizeof header+8) {
            uint64_t ip = 0;
            for (size_t i = 0; i < 8; ++i) reinterpret_cast<unsigned char*>(&ip)[i] = data[(tail+sizeof header+i) & mask];
            ++counts[ip];
        }
        if (!header.size) break;
        tail += header.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

void watcher() {
    if (!in_executable(bridge_object, 0x30)) {
        log("X4VR watch: X4's head-tracker bridge address isn't in X4 (only Linux 9.00 is known); no watch");
        return;
    }
    // X4 constructs the bridge (stores its vtable) when it starts applying head tracking; wait for both.
    const auto vtable = [] { return *reinterpret_cast<const volatile uintptr_t*>(bridge_object); };
    for (int i = 0; i < 1200 && !(game_state().head_tracking && vtable() == bridge_vtable); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (vtable() != bridge_vtable) {
        char line[160];
        std::snprintf(line, sizeof line, "X4VR watch: no head-tracker bridge after 10 min (head tracking %s, value 0x%llx); no watch",
                      game_state().head_tracking ? "on" : "off", static_cast<unsigned long long>(vtable()));
        log(line);
        return;
    }

    log("X4VR watch: recording which code reads the head position (bridge +0x10..+0x1f) for 20 s");
    const long page = sysconf(_SC_PAGESIZE);
    std::map<pid_t, std::vector<Watch>> watches;
    std::map<uint64_t, uint64_t> counts;
    std::string first_error;
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(20);
    for (auto next_scan = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() < end;) {
        if (std::chrono::steady_clock::now() >= next_scan) { // new threads appear: watch them too
            next_scan += std::chrono::seconds(2);
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
                const pid_t tid = pid_t(std::atoi(entry.path().filename().c_str()));
                if (!tid || watches.count(tid)) continue;
                auto& list = watches[tid];
                for (const uintptr_t address : {bridge_object+0x10, bridge_object+0x18}) {
                    std::string error;
                    Watch w;
                    w.fd = open_watch(tid, address, error);
                    if (w.fd < 0) { if (first_error.empty()) first_error = error; continue; }
                    w.size = size_t(page)*(ring_pages+1);
                    w.ring = mmap(nullptr, w.size, PROT_READ | PROT_WRITE, MAP_SHARED, w.fd, 0);
                    if (w.ring == MAP_FAILED) { close(w.fd); continue; }
                    list.push_back(w);
                }
            }
        }
        for (auto& [tid, list] : watches) for (auto& w : list) drain(w, counts);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    size_t opened = 0;
    for (auto& [tid, list] : watches) for (auto& w : list) { drain(w, counts); munmap(w.ring, w.size); close(w.fd); ++opened; }
    if (!opened) { log("X4VR watch: no watchpoint could be set ("+first_error+"); check /proc/sys/kernel/perf_event_paranoid"); return; }
    std::vector<std::pair<uint64_t, uint64_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    log("X4VR watch: "+std::to_string(sorted.size())+" code addresses touched the head position ("+std::to_string(opened)+" watchpoints):");
    for (size_t i = 0; i < sorted.size() && i < 40; ++i) {
        char line[96];
        std::snprintf(line, sizeof line, "X4VR watch:   0x%llx  %llu times", static_cast<unsigned long long>(sorted[i].first),
                      static_cast<unsigned long long>(sorted[i].second));
        log(line);
    }
}
}

void start_head_watch() {
    const char* on = std::getenv("X4VR_WATCH_HEAD");
    if (!on || *on != '1') return;
    std::thread(watcher).detach();
}
}
