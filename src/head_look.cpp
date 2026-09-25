#include <x4vr/head_look.hpp>

namespace x4vr::experimental {
bool HeadLook::apply(const NativePose& base, const Matrix& head, bool valid, NativePose& output) {
    if (!enabled_ || !valid || !is_rigid(head)) return false;
    Matrix native;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) native.m[r][c] = base[c*4+r];
    if (!is_rigid(native)) return false;
    // Same yaw-only origin convention as the stereo Session. Pitch/roll remain
    // relative to gravity. Ignore translation until X4 units/metre are verified.
    if (recenter_) {
        origin_inverse_ = inverse_rigid(seated_origin(head));
        for (int r = 0; r < 3; ++r) origin_inverse_.m[r][3] = 0;
        recenter_ = false;
    }
    auto orientation = head;
    for (int r = 0; r < 3; ++r) orientation.m[r][3] = 0;
    const auto relative = multiply(origin_inverse_, orientation);
    auto basis = Matrix::identity(); basis.m[2][2] = -1;
    const auto local = multiply(multiply(basis, relative), basis);
    NativePose next = base; // preserve world translation/bottom row bit-for-bit
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        double value = 0;
        for (int k = 0; k < 3; ++k) value += double(native.m[r][k])*local.m[k][c];
        next[c*4+r] = static_cast<float>(value);
    }
    output = next;
    return true;
}
}
