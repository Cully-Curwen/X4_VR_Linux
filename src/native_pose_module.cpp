// Startup-only native camera adapter. Default is observation-only. Explicit
// head-look mode composes tracked orientation; never changes projection or scale.
#include "native_camera.hpp"
#include <x4vr/pose_detour.hpp>
#include <x4vr/copy_call_hook.hpp>
#include <x4vr/scene_camera_guard.hpp>
#include <x4vr/head_look.hpp>
#include <x4vr/pose_control.hpp>
#include <x4vr/runtime_bootstrap.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include <cmath>
#include <iterator>

namespace {
using namespace x4vr::experimental;
struct Capture {
    HANDLE file = INVALID_HANDLE_VALUE;
    uintptr_t camera{};
    uintptr_t image_base{};
    PoseFunction rebuild{};
    std::atomic_uint64_t calls{};
    std::atomic_bool failed{};
    std::unique_ptr<PoseControls> controls;
    std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
    HeadLook head_look;
    std::mutex look_mutex;
    bool stopped{}, tracking_lost{}, applied_once{};
    std::atomic_bool scene_applied{};
    std::atomic_bool scene_seen{};
    // Diagnostic target selection, re-read from control.txt at most every 250 ms:
    // "targets=A|B|AB|none source=off|yaw|head". A = pool Zone copy, B = derived global.
    std::filesystem::path control_path;
    std::atomic_uint64_t control_tick{};
    std::atomic_int targets{}, source{};
    std::atomic_bool derived_applied{}, head_enable{}, context_applied{};
    ~Capture() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
struct Sample {
    uint64_t sequence{}, tick{}, camera{};
    uint32_t thread{}, reserved{};
    NativePose pose{};
};
static_assert(sizeof(Sample) == 96);
bool track(Capture& capture, const NativePose& input, NativePose& output) {
    if (!capture.controls) return false;
    std::unique_lock lock(capture.look_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    auto command = capture.controls->poll();
    if (command == PoseCommand::none && capture.head_enable.exchange(false)) command = PoseCommand::enable;
    if (command == PoseCommand::shutdown) {
        capture.head_look.disable(); capture.stopped = true;
        capture.runtime.reset();
        OutputDebugStringA("X4VR head look: stopped; native poses restored, runtime reference released\n");
    } else if (command == PoseCommand::disable) {
        capture.head_look.disable();
        OutputDebugStringA("X4VR head look: disabled; native poses restored\n");
    } else if (command == PoseCommand::enable && !capture.stopped) {
        capture.head_look.enable(); capture.applied_once = false;
        OutputDebugStringA("X4VR head look: enabled; waiting for valid tracking and X4 foreground\n");
    } else if (command == PoseCommand::recenter && !capture.stopped) {
        capture.head_look.recenter();
        OutputDebugStringA("X4VR head look: yaw recenter requested\n");
    }
    if (capture.stopped || !capture.runtime) return false;
    try {
        x4vr::Matrix head;
        const auto status = capture.runtime->sample_tracking(head);
        if (status == x4vr::FrameStatus::quit_requested) {
            capture.head_look.disable(); capture.stopped = true;
            capture.runtime.reset();
            OutputDebugStringA("X4VR head look: runtime quit; native poses restored\n");
            return false;
        }
        const bool valid = status == x4vr::FrameStatus::ready;
        if (!valid && !capture.tracking_lost) OutputDebugStringA("X4VR head look: tracking unavailable; native pose pass-through\n");
        if (valid && capture.tracking_lost) OutputDebugStringA("X4VR head look: tracking recovered\n");
        capture.tracking_lost = !valid;
        DWORD foreground{};
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
        if (foreground != GetCurrentProcessId()) return false;
        const bool applied = capture.head_look.apply(input, head, valid, output);
        if (applied && !capture.applied_once) {
            OutputDebugStringA("X4VR head look: tracked orientation prepared; translation unchanged, visible effect unverified\n");
            capture.applied_once = true;
        }
        return applied;
    } catch (const std::exception& error) {
        capture.head_look.disable();
        OutputDebugStringA("X4VR head look: exception; tracking disabled, native pose pass-through\n");
        OutputDebugStringA(error.what());
        return false;
    }
}
void refresh_mode(Capture& capture) {
    const auto now = GetTickCount64();
    auto last = capture.control_tick.load(std::memory_order_relaxed);
    if (now-last < 250 || !capture.control_tick.compare_exchange_strong(last, now)) return;
    std::ifstream file(capture.control_path);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (text.empty()) return;
    const auto value = [&](const char* key) {
        const auto at = text.find(key);
        if (at == std::string::npos) return std::string{};
        const auto begin = at+std::strlen(key);
        return text.substr(begin, text.find_first_of(" \r\n\t", begin)-begin);
    };
    const auto t = value("targets="), s = value("source=");
    const int targets = (t.find('A') != std::string::npos ? 1 : 0) | (t.find('B') != std::string::npos ? 2 : 0) | (t.find('S') != std::string::npos ? 4 : 0) | (t.find('C') != std::string::npos ? 8 : 0);
    const int source = s == "yaw" ? 1 : s == "head" ? 2 : 0;
    if (capture.targets.exchange(targets) != targets || capture.source.exchange(source) != source) {
        if (source == 2) capture.head_enable.store(true);
        OutputDebugStringA(("X4VR diagnostic mode: targets="+t+" source="+s+"\n").c_str());
    }
}
// Local yaw about the camera's own up axis; translation kept bit-for-bit.
NativePose yaw_test(const NativePose& input, double degrees) {
    const double r = degrees*3.14159265358979323846/180, c = std::cos(r), s = std::sin(r);
    const double rotation[3][3]{{c,0,s},{0,1,0},{-s,0,c}};
    NativePose output = input;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) {
        double value = 0;
        for (int k = 0; k < 3; ++k) value += double(input[k*4+row])*rotation[k][col];
        output[col*4+row] = static_cast<float>(value);
    }
    return output;
}
bool compute(Capture& capture, int target, const NativePose& input, NativePose& output) {
    refresh_mode(capture);
    if (!(capture.targets.load() & target)) return false;
    switch (capture.source.load()) {
    case 1: output = yaw_test(input, 30); return true;
    case 2: return track(capture, input, output);
    default: return false;
    }
}
bool record(Capture& capture, uintptr_t camera, const NativePose& input, NativePose& output, int target) {
    const auto sequence = capture.calls.fetch_add(1, std::memory_order_relaxed)+1;
    if (!capture.failed.load(std::memory_order_relaxed) && sequence <= 30720
        && (sequence <= 64 || sequence % 120 == 0)) {
        const Sample sample{sequence, GetTickCount64(), camera, GetCurrentThreadId(), 0, input};
        DWORD written{};
        if (!WriteFile(capture.file, &sample, sizeof(sample), &written, nullptr) || written != sizeof(sample)) {
            capture.failed.store(true, std::memory_order_relaxed);
            OutputDebugStringA("X4VR native pose: capture write failed\n");
        } else if (sequence == 1) {
            OutputDebugStringA("X4VR native pose: first selected-camera input captured\n");
        }
    }
    return compute(capture, target, input, output); // false unless a diagnostic mode is set
}
bool observe(const NativePose& input, NativePose& output, void* opaque) {
    auto& capture = *static_cast<Capture*>(opaque);
    const bool applied = record(capture, capture.camera, input, output, 2);
    if (applied && !capture.derived_applied.exchange(true))
        OutputDebugStringA("X4VR derived pose: modified pose passed to native rebuild of 0x6d1d660\n");
    return applied;
}
bool read(uintptr_t address, void* output, size_t size) {
    SIZE_T count{};
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                                       output, size, &count) && count == size;
}
void scene_copied(void* destination, const void* source, size_t size, void* opaque) {
    auto& capture = *static_cast<Capture*>(opaque);
    if (!capture.scene_seen.exchange(true))
        OutputDebugStringA("X4VR scene copy: first producer copy intercepted; original copy completed\n");
    if (size != 0xd10) return;
    const auto address = reinterpret_cast<uintptr_t>(destination);
    uintptr_t manager{}, pool{};
    uint32_t read_half{}, count{};
    std::array<char, 64> label{};
    if (!read(capture.image_base+0x6cf1578, &manager, sizeof(manager)) || !manager ||
        !read(manager+0x1f0, &pool, sizeof(pool)) ||
        !read(manager+0x1e8, &count, sizeof(count)) ||
        !read(capture.image_base+0x6b66280, &read_half, sizeof(read_half))) return;
    // Reject pool/stride/count before reading the label outside the camera record.
    if (!scene_camera_destination(address, size, pool, read_half, count, "class U::Zone")) return;
    if (!read(address+0xefc, label.data(), label.size())) return;
    const auto length = strnlen_s(label.data(), label.size());
    if (!scene_camera_destination(address, size, pool, read_half, count,
                                  std::string_view(label.data(), length))) return;
    NativePose input{}, output{};
    if (!read(address, input.data(), sizeof(input))) return;
    if (!record(capture, address, input, output, 1|4|8)) return;
    // Rebuild on an aligned private copy first. The native routine recomputes
    // view, projections/VP and visibility data. Do not mutate the game's source
    // stack camera, nor accumulate from the destination's previous frame.
    alignas(16) std::array<std::byte, 0xd10> next{};
    if (!read(address, next.data(), next.size())) return;
    capture.rebuild(next.data(), &output);
    std::memcpy(destination, next.data(), next.size());
    // Target C: world-space context camera, copied unmodified before this call
    // at 0x77a2ec. Same rotation as the Zone view, plus world translation.
    uintptr_t context{};
    if ((capture.targets.load() & 8) && read(capture.image_base+0x6cf1908, &context, sizeof(context)) && context &&
        read(context+0x30, &context, sizeof(context)) && context) {
        alignas(16) std::array<std::byte, 0xd10> world{};
        NativePose world_pose{};
        if (read(context+0x350, world.data(), world.size())) {
            std::memcpy(world_pose.data(), world.data(), sizeof(world_pose));
            bool fresh = true; // only when ctx still holds this frame's unrotated pose
            for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r)
                fresh = fresh && std::fabs(world_pose[c*4+r]-input[c*4+r]) < 1e-4f;
            if (fresh) {
                for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) world_pose[c*4+r] = output[c*4+r];
                capture.rebuild(world.data(), &world_pose);
                std::memcpy(reinterpret_cast<void*>(context+0x350), world.data(), world.size());
                if (!capture.context_applied.exchange(true))
                    OutputDebugStringA("X4VR context camera: rotated world-space ctx+0x350 committed\n");
            }
        }
    }
    // Target S: the producer keeps using its stack camera after this copy.
    if (capture.targets.load() & 4) std::memcpy(const_cast<void*>(source), next.data(), next.size());
    if (!capture.scene_applied.exchange(true))
        OutputDebugStringA("X4VR scene copy: rebuilt tracked camera committed to guarded Zone view; visible effect unverified\n");
}
}
extern "C" __declspec(dllexport) DWORD WINAPI X4VR_Startup(void*) {
    static std::atomic_flag attempted{};
    if (attempted.test_and_set()) return 30;
    try {
        if (!x4vr::observe::supported_executable()) {
            OutputDebugStringA("X4VR native pose: unsupported executable; no hook installed\n");
            return 31;
        }
        wchar_t root[32768]{};
        const auto length = GetEnvironmentVariableW(L"X4VR_CAPTURE_DIR", root, 32768);
        if (!length || length >= 32768) return 32;
        const auto directory = std::filesystem::path(root)/(L"pose-hook-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(directory)) return 33;
        auto capture = std::make_unique<Capture>();
        capture->image_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        wchar_t head_look_flag[2]{};
        const bool head_look = GetEnvironmentVariableW(L"X4VR_HEAD_LOOK", head_look_flag, 2) == 1 && head_look_flag[0] == L'1';
        wchar_t scene_flag[2]{};
        const bool scene = GetEnvironmentVariableW(L"X4VR_SCENE_COPY", scene_flag, 2) == 1 && scene_flag[0] == L'1';
        if (head_look) {
            capture->controls = std::make_unique<PoseControls>(GetCurrentProcessId());
            capture->runtime = x4vr::acquire_runtime_bootstrap();
        }
        capture->camera = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x6d1d660;
        capture->control_path = directory/L"control.txt";
        capture->file = CreateFileW((directory/L"pose-input.bin").c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (capture->file == INVALID_HANDLE_VALUE) return 34;
        std::ofstream description(directory/L"capture.txt");
        description << (head_look ? "Head-look capable native hook; starts DISABLED. Orientation only; no stereo.\n"
                                 : "Observation-only native pose hook; input pointer and pose remain unchanged.\n")
            << "executable_sha256=19750a6563889a970f434b5566eb396c6b2dc29ff814bd3e336f838176ad6891\n"
            << (scene ? "call_site_rva=0x77a376 target_rva=0x19efe90 guard=write-half-pool-slot+class-U::Zone\n"
                      : "target_rva=0xf41000 selected_camera_rva=0x6d1d660\n")
            << "binary_record=96 bytes: u64 sequence,tick_ms,camera; u32 thread,reserved; f32 column_major_pose[16]\n"
            << "sampling=first64 then every120 up to sequence30720; valid rigid inputs only\n";
        description.flush();
        if (!description) return 35;
        // This module and its callback/trampoline stay alive until process exit.
        // No shutdown work runs from DllMain or a C++ static destructor.
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&X4VR_Startup), &pinned)) return 36;
        auto hook = std::make_unique<PoseDetour>();
        constexpr std::array code{std::byte{0x48},std::byte{0x8b},std::byte{0xc4},std::byte{0x48},std::byte{0x81},
            std::byte{0xec},std::byte{0x68},std::byte{0x01},std::byte{0x00},std::byte{0x00}};
        const auto target = reinterpret_cast<PoseFunction>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0xf41000);
        if (scene) {
            // Exact loaded bytes guard both the sole replaced call's argument
            // setup and the native rebuild entry. Hash verification is above.
            constexpr std::array setup{std::byte{0x49},std::byte{0x8d},std::byte{0x4d},std::byte{0x10},
                std::byte{0x48},std::byte{0x8d},std::byte{0x95},std::byte{0x60},std::byte{0x08},std::byte{0x00},std::byte{0x00},
                std::byte{0x41},std::byte{0xb8},std::byte{0x10},std::byte{0x0d},std::byte{0x00},std::byte{0x00}};
            std::array<std::byte, setup.size()> loaded_setup{};
            std::array<std::byte, code.size()> loaded_code{};
            if (!read(capture->image_base+0x77a365, loaded_setup.data(), loaded_setup.size()) || loaded_setup != setup ||
                !read(capture->image_base+0xf41000, loaded_code.data(), loaded_code.size()) || loaded_code != code)
                throw std::runtime_error("Scene-copy/rebuild loaded signature mismatch");
            capture->rebuild = target;
            auto scene_hook = std::make_unique<CopyCallHook>();
            scene_hook->install_quiescent(reinterpret_cast<void*>(capture->image_base+0x77a376),
                reinterpret_cast<CopyFunction>(capture->image_base+0x19efe90), scene_copied, capture.get());
            // Also detour the derived-global rebuild (target B); the private-copy
            // rebuild above passes through because its camera pointer differs.
            hook->install_quiescent(target, code, reinterpret_cast<void*>(capture->camera), observe, capture.get());
            capture.release(); scene_hook.release(); hook.release();
            OutputDebugStringA("X4VR scene copy: startup hooks A(pool Zone copy)+B(derived pose) installed; control.txt selects, default none\n");
            return 0;
        }
        hook->install_quiescent(target, code, reinterpret_cast<void*>(capture->camera), observe, capture.get());
        capture.release(); hook.release(); // deliberate process lifetime, reclaimed by OS
        OutputDebugStringA(head_look ? "X4VR native pose: startup hook installed, head-look prepared but DISABLED\n"
                                   : "X4VR native pose: startup hook installed, observation only\n");
        return 0;
    } catch (const std::exception& error) {
        OutputDebugStringA(error.what());
        return 37;
    }
}
