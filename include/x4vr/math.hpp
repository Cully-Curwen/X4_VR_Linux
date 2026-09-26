#pragma once
#include <openvr.h>

namespace x4vr {
// Row-major storage, column vectors; right handed, +Y up, forward -Z.
// All translations and clip distances are metres, independent of X4 world units.
struct Matrix {
    float m[4][4]{};
    static Matrix identity();
};
Matrix from_pose(const vr::HmdMatrix34_t& pose);
Matrix multiply(const Matrix& a, const Matrix& b);
Matrix inverse_rigid(const Matrix& pose);
bool is_rigid(const Matrix& pose);
Matrix seated_origin(const Matrix& head);
Matrix vulkan_projection(float left, float right, float top, float bottom,
                         float near_m, float far_m, bool reverse_z = false);
// Turn compensation (alternate-eye rendering). Submit pose for an eye image rendered at head
// pose `own` with game view matrix `view` (row-major world-to-camera, X4 camera axes x right,
// y up, z forward), when the newest image (the other eye's) was rendered at head pose `newest`
// with `newest_view`: the newest head pose turned by the game camera's rotation from that
// frame to this one, keeping `own`'s position. Equals `own` when only the head moved.
Matrix turned_pose(const Matrix& newest, const Matrix& newest_eye, const Matrix& newest_view,
                   const Matrix& own, const Matrix& own_eye, const Matrix& view);
double rotation_degrees(const Matrix& a, const Matrix& b); // angle between the rotation parts
}
