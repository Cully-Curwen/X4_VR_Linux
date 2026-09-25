#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace x4vr::experimental {
// Column-major native world-from-camera pose. This is NOT a native camera record.
using NativePose = std::array<float, 16>;
// Hypothesized Win64 two-pointer ABI. Only the owned assembly fixture has been
// tested. Do not use this typedef as proof of X4's ABI or return-value contract.
using PoseFunction = uintptr_t (*)(void* camera, const NativePose* pose);
// Input is a copy. Returning false, throwing a C++ exception, or producing a
// non-rigid pose forwards the original pointer unchanged. No SEH suppression.
using PoseTransform = bool (*)(const NativePose& input, NativePose& output, void* context);

// An experimental trampoline mechanism, not an X4 integration or injector.
// Installation/removal/destruction MUST run on the same managing thread, outside
// DllMain/loader callbacks, with ALL target callers quiescent (including callbacks
// and threads approaching the target). An in-flight counter cannot prove that.
// Once installed, concurrent calls are supported; callback/context must be thread
// safe and remain alive until removal. Never unload the hosting module while live.
// The target must consume its pose synchronously, without retaining the pointer.
// Only one instance may own a hook per copy of this library. No wildcard unhook.
class PoseDetour final {
public:
    PoseDetour() = default;
    ~PoseDetour() noexcept;
    PoseDetour(const PoseDetour&) = delete;
    PoseDetour& operator=(const PoseDetour&) = delete;
    // Exact code bytes are a conflict guard, not an executable/version identity
    // check. A future game adapter MUST independently verify the pinned X4 image.
    void install_quiescent(PoseFunction target, std::span<const std::byte> expected_code,
                           void* selected_camera, PoseTransform transform, void* context);
    void remove_quiescent();
    bool installed() const noexcept { return enabled_; }
private:
    static uintptr_t dispatch(void* camera, const NativePose* pose);
    PoseFunction target_{}, original_{};
    PoseTransform transform_{};
    void* selected_camera_{};
    void* context_{};
    unsigned long owner_thread_{};
    bool initialized_{}, created_{}, enabled_{};
};
}
