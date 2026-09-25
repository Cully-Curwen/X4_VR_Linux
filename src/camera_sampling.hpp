#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>

namespace x4vr::observe {
inline std::optional<uint64_t> camera_signature(const std::byte* data, size_t size) {
    if (size < 576) return {};
    std::array<float, 32> values{};
    std::memcpy(values.data(), data, sizeof(values));
    for (float value : values) if (!std::isfinite(value)) return {};
    // Skip uninitialised/no-camera UI blocks. This is only a sample filter,
    // not proof that a matrix belongs to the player's camera.
    if (std::abs(values[15]-1.0f) > 1e-3f || std::abs(values[16]) < 1e-8f || std::abs(values[21]) < 1e-8f) return {};
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(values); ++i) { hash ^= std::to_integer<unsigned char>(data[i]); hash *= 1099511628211ull; }
    return hash;
}
class CameraSampling {
    std::mutex mutex_;
    uint64_t epoch_ = ~0ull;
    unsigned attempts_{}, accepted_{};
    std::array<uint64_t, 16> signatures_{};
    unsigned signature_count_{};
public:
    // Bounded work even when no valid cameras are present; sample several views
    // instead of consuming the epoch on the first (often empty) descriptor bind.
    bool attempt(uint64_t present) {
        std::lock_guard lock(mutex_);
        const auto epoch = present/120;
        if (epoch_ != epoch) { epoch_ = epoch; attempts_ = 0; signature_count_ = 0; }
        if (accepted_ >= 4096 || signature_count_ >= signatures_.size() || attempts_ >= 512) return false;
        ++attempts_; return true;
    }
    std::optional<unsigned> accept(uint64_t present, uint64_t signature) {
        std::lock_guard lock(mutex_);
        if (epoch_ != present/120 || accepted_ >= 4096 || signature_count_ >= signatures_.size()) return {};
        for (unsigned i = 0; i < signature_count_; ++i) if (signatures_[i] == signature) return {};
        signatures_[signature_count_++] = signature;
        return accepted_++;
    }
};
}
