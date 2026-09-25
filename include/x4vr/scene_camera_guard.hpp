#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
namespace x4vr::experimental {
// Additional constraints for the pinned 0x77a376 producer call only. A label
// alone never authorizes a write. The pool has two halves of 100 view objects.
inline bool scene_camera_destination(uintptr_t destination, size_t copy_size,
                                     uintptr_t pool, uint32_t read_half,
                                     uint32_t produced_count, std::string_view label) noexcept {
    constexpr uintptr_t stride = 0xf80, per_half = 100;
    if (!pool || pool > std::numeric_limits<uintptr_t>::max()-2*per_half*stride ||
        copy_size != 0xd10 || read_half > 1 || !produced_count || produced_count > per_half ||
        label != "class U::Zone" || destination < pool) return false;
    const auto offset = destination-pool;
    if (offset < 0x10 || (offset-0x10)%stride) return false;
    const auto slot = (offset-0x10)/stride;
    const auto first = static_cast<uintptr_t>(read_half ^ 1)*per_half;
    return slot >= first && slot-first < produced_count;
}
}
