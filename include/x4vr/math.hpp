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
}
