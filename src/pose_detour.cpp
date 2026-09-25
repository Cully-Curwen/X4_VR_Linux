#include <x4vr/pose_detour.hpp>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace x4vr::experimental {
namespace {
std::atomic<PoseDetour*> active{};
thread_local bool in_transform{};
void require(MH_STATUS status, const char* operation) {
    if (status != MH_OK) throw std::runtime_error(std::string(operation)+": "+MH_StatusToString(status));
}
bool rigid(const NativePose& pose) noexcept {
    for (float value : pose) if (!std::isfinite(value)) return false;
    for (size_t c = 0; c < 4; ++c)
        if (std::abs(pose[c*4+3]-(c == 3 ? 1.f : 0.f)) > 1e-5f) return false;
    for (size_t a = 0; a < 3; ++a) for (size_t b = 0; b < 3; ++b) {
        double dot = 0;
        for (size_t r = 0; r < 3; ++r) dot += double(pose[a*4+r])*pose[b*4+r];
        if (std::abs(dot-(a == b ? 1.0 : 0.0)) > 0.005) return false;
    }
    const double determinant = double(pose[0])*(double(pose[5])*pose[10]-double(pose[9])*pose[6])
        - double(pose[4])*(double(pose[1])*pose[10]-double(pose[9])*pose[2])
        + double(pose[8])*(double(pose[1])*pose[6]-double(pose[5])*pose[2]);
    return std::abs(determinant-1.0) <= 0.005;
}
struct TransformScope {
    TransformScope() { in_transform = true; }
    ~TransformScope() { in_transform = false; }
};
}
PoseDetour::~PoseDetour() noexcept {
    // The owner must provide quiescence even for this fallback. Failure cannot
    // safely discard an object still reachable from patched executable code.
    try { remove_quiescent(); } catch (...) { std::terminate(); }
}
void PoseDetour::install_quiescent(PoseFunction target, std::span<const std::byte> code,
                                   void* camera, PoseTransform transform, void* context) {
    if (!target || !camera || !transform || code.size() < 10 || code.size() > 64)
        throw std::invalid_argument("Missing pose-hook target, identity, callback or code signature");
    if (initialized_ || created_) throw std::logic_error("Pose hook already owned");
    std::array<std::byte, 64> actual{};
    SIZE_T read{};
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(target), actual.data(), code.size(), &read)
        || read != code.size() || std::memcmp(actual.data(), code.data(), code.size()))
        throw std::runtime_error("Pose-hook code signature mismatch or unreadable target");
    PoseDetour* expected = nullptr;
    if (!active.compare_exchange_strong(expected, this, std::memory_order_acq_rel))
        throw std::logic_error("Another pose hook is owned");
    owner_thread_ = GetCurrentThreadId();
    try {
        require(MH_Initialize(), "Initialize private MinHook");
        initialized_ = true;
        target_ = target; selected_camera_ = camera; transform_ = transform; context_ = context;
        void* trampoline{};
        require(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&dispatch), &trampoline), "Create pose hook");
        created_ = true;
        original_ = reinterpret_cast<PoseFunction>(trampoline);
        // All immutable dispatch state is ready before publishing the code patch.
        active.store(this, std::memory_order_release);
        require(MH_EnableHook(reinterpret_cast<void*>(target)), "Enable pose hook");
        enabled_ = true;
    } catch (...) {
        remove_quiescent();
        throw;
    }
}
void PoseDetour::remove_quiescent() {
    if (active.load(std::memory_order_acquire) != this) return;
    if (GetCurrentThreadId() != owner_thread_) throw std::logic_error("Pose-hook owner thread changed");
    if (enabled_) {
        require(MH_DisableHook(reinterpret_cast<void*>(target_)), "Disable pose hook");
        enabled_ = false;
    }
    if (created_) {
        require(MH_RemoveHook(reinterpret_cast<void*>(target_)), "Remove pose hook");
        created_ = false;
    }
    if (initialized_) {
        require(MH_Uninitialize(), "Uninitialize private MinHook");
        initialized_ = false;
    }
    original_ = target_ = nullptr;
    transform_ = nullptr; selected_camera_ = context_ = nullptr;
    active.store(nullptr, std::memory_order_release);
}
uintptr_t PoseDetour::dispatch(void* camera, const NativePose* pose) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->original_) std::terminate(); // broken lifecycle contract
    if (camera == self->selected_camera_ && pose && !in_transform) {
        NativePose input;
        std::memcpy(input.data(), pose, sizeof(input));
        if (rigid(input)) {
            NativePose output = input;
            bool accepted = false;
            {
                TransformScope scope;
                try { accepted = self->transform_(input, output, self->context_); }
                catch (...) { accepted = false; }
            }
            if (accepted && rigid(output)) return self->original_(camera, &output);
        }
    }
    return self->original_(camera, pose);
}
}
