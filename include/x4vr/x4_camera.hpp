#pragma once
#include <x4vr/session.hpp>
#include <span>
#include <cstddef>

namespace x4vr {
struct X4Plane {
    double x{}, y{}, z{}, w{}; // normalized inward-facing world-space half-space
};
// Typed matrices for a future per-eye scene pass. NOT a patchable native camera
// object: culling, other native fields, per-object WVP and histories still need
// their own per-eye integration. Base records use the verified retail offsets.
struct X4EyeCamera {
    Matrix view;
    Matrix inverse_view;
    Matrix projection_native; // unjittered, +Z forward, Y up, infinite reverse Z
    Matrix projection_vulkan; // current jitter applied, positive-height viewport
    Matrix view_projection;
    Matrix projection_culling_native; // finite forward-depth projection at +0xc0
    Matrix culling_view_projection;   // native +0x100 counterpart
    std::array<X4Plane, 6> visibility_planes; // left/right/bottom/top/near/far
};
// Explicit scale: no assumption that one X4 unit is one physical metre. The
// caller must identify the player camera and calibrate scale before using this
// in a render pass. EyeView supplies the full tracked/canted seated_from_eye.
X4EyeCamera make_x4_eye_camera(std::span<const std::byte> native_camera,
                              const EyeView& runtime_eye, float units_per_metre);
// Conservative binocular visibility: visible to EITHER eye. This does not call
// or replace X4's culling system. Unknown/nonfinite bounds fail open (draw them).
bool x4_stereo_sphere_visible(const std::array<X4EyeCamera, 2>& eyes,
                              double x, double y, double z, double radius);
}
