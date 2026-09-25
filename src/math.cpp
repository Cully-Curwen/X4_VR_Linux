#include <x4vr/math.hpp>
#include <cmath>
#include <stdexcept>

namespace x4vr {
Matrix Matrix::identity() {
    Matrix r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
}
Matrix from_pose(const vr::HmdMatrix34_t& pose) {
    auto r = Matrix::identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = pose.m[i][j];
    return r;
}
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
bool is_rigid(const Matrix& p) {
    for (const auto& row : p.m)
        for (float v : row) if (!std::isfinite(v)) return false;
    constexpr float tolerance = 0.002f;
    for (int i = 0; i < 4; ++i)
        if (std::abs(p.m[3][i] - (i == 3 ? 1.f : 0.f)) > tolerance) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float dot = 0;
            for (int k = 0; k < 3; ++k) dot += p.m[k][i] * p.m[k][j];
            if (std::abs(dot - (i == j ? 1.f : 0.f)) > tolerance) return false;
        }
    const float det = p.m[0][0] * (p.m[1][1]*p.m[2][2] - p.m[1][2]*p.m[2][1])
                    - p.m[0][1] * (p.m[1][0]*p.m[2][2] - p.m[1][2]*p.m[2][0])
                    + p.m[0][2] * (p.m[1][0]*p.m[2][1] - p.m[1][1]*p.m[2][0]);
    return std::abs(det - 1.f) <= tolerance;
}
Matrix inverse_rigid(const Matrix& p) {
    if (!is_rigid(p)) throw std::invalid_argument("Expected a finite rigid pose");
    auto r = Matrix::identity();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r.m[i][j] = p.m[j][i];
        for (int j = 0; j < 3; ++j) r.m[i][3] -= r.m[i][j] * p.m[j][3];
    }
    return r;
}
Matrix seated_origin(const Matrix& head) {
    if (!is_rigid(head)) throw std::invalid_argument("Invalid head pose");
    // Recenter position and yaw only, preserving gravity and head pitch/roll.
    const float yaw = std::atan2(head.m[0][2], head.m[2][2]);
    auto origin = Matrix::identity();
    origin.m[0][0] = origin.m[2][2] = std::cos(yaw);
    origin.m[0][2] = std::sin(yaw);
    origin.m[2][0] = -std::sin(yaw);
    for (int i = 0; i < 3; ++i) origin.m[i][3] = head.m[i][3];
    return origin;
}
Matrix vulkan_projection(float l, float r, float t, float b, float n, float f, bool reverse) {
    for (float v : {l, r, t, b, n, f})
        if (!std::isfinite(v)) throw std::invalid_argument("Non-finite projection");
    if (!(l < r && t < b && n > 0 && f > n))
        throw std::invalid_argument("Invalid frustum or clipping planes");
    Matrix p;
    p.m[0][0] = 2.f / (r-l);
    p.m[0][2] = (r+l) / (r-l);
    // Positive-height Vulkan viewport: flip the runtime's Y-up projection.
    p.m[1][1] = -2.f / (b-t);
    p.m[1][2] = -(b+t) / (b-t);
    p.m[2][2] = reverse ? n / (f-n) : -f / (f-n);
    p.m[2][3] = reverse ? f*n / (f-n) : -f*n / (f-n);
    p.m[3][2] = -1.f;
    return p;
}
}
