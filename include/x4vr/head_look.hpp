#pragma once
#include <x4vr/math.hpp>
#include <x4vr/pose_detour.hpp>

namespace x4vr::experimental {
// Orientation-only native camera integration stage, not a stereo renderer.
// Caller serializes state. Invalid/lost tracking returns false and does not
// overwrite output or consume a pending recenter. No cached pose is reapplied.
class HeadLook {
public:
    void enable() noexcept { enabled_ = true; recenter_ = true; }
    void disable() noexcept { enabled_ = false; }
    void recenter() noexcept { recenter_ = true; }
    bool enabled() const noexcept { return enabled_; }
    bool apply(const NativePose& base, const Matrix& head, bool tracking_valid, NativePose& output);
private:
    bool enabled_{}, recenter_ = true;
    Matrix origin_inverse_ = Matrix::identity();
};
}
