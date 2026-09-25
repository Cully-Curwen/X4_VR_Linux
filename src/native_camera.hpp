#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace x4vr::observe {
// This only reconstructs registers; it never transfers control or alters the stack.
bool context_at_return(uintptr_t address, CONTEXT& output) noexcept;
bool supported_executable() noexcept;
struct NativeCamera {
    const char* status = "disabled";
    uintptr_t wrapper{}, camera{}, temporary{};
    std::array<std::byte, 0x80> wrapper_bytes{};
    std::array<std::byte, 0xce0> camera_bytes{};
    std::array<std::byte, 0x700> uniform_bytes{};
    bool wrapper_read{}, camera_read{}, uniform_read{};
};
NativeCamera capture_native_camera() noexcept;
}
