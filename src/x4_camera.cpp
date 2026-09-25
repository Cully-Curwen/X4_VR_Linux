#include <x4vr/x4_camera.hpp>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace x4vr {
namespace {
Matrix load(std::span<const std::byte> bytes, size_t offset) {
    Matrix result;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            std::memcpy(&result.m[row][column], bytes.data()+offset+(column*4+row)*sizeof(float), sizeof(float));
            if (!std::isfinite(result.m[row][column])) throw std::invalid_argument("Nonfinite X4 camera matrix");
        }
    return result;
}
float scalar(std::span<const std::byte> bytes, size_t offset) {
    float value;
    std::memcpy(&value, bytes.data()+offset, sizeof(value));
    if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite X4 clip distance");
    return value;
}
// Accumulate in double precision: world translations can already exceed 30,000
// units while physical eye separation is only centimetres.
Matrix product(const Matrix& a, const Matrix& b) {
    Matrix result;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        double value = 0;
        for (int k = 0; k < 4; ++k) value += double(a.m[r][k])*b.m[k][c];
        result.m[r][c] = static_cast<float>(value);
        if (!std::isfinite(result.m[r][c])) throw std::invalid_argument("Eye camera overflow");
    }
    return result;
}
bool near(float a, float b) { return std::abs(a-b) < 1e-5f; }
void require_projection(const Matrix& p, bool native) {
    for (const auto& row : p.m) for (float v : row)
        if (!std::isfinite(v)) throw std::invalid_argument("Nonfinite eye projection");
    if (!(p.m[0][0] > 0 && (native ? p.m[1][1] > 0 : p.m[1][1] < 0)) ||
        !near(p.m[3][2], native ? 1.f : -1.f))
        throw std::invalid_argument("Unsupported projection convention");
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        const bool allowed = (r == 0 && (c == 0 || c == 2)) ||
                             (r == 1 && (c == 1 || c == 2)) ||
                             (r == 2 && (c == 2 || c == 3)) || (r == 3 && c == 2);
        if (!allowed && !near(p.m[r][c], 0)) throw std::invalid_argument("Unsupported projection shape");
    }
    if (native && (!near(p.m[2][2], 0) || !(p.m[2][3] > 0)))
        throw std::invalid_argument("X4 camera is not infinite reverse Z");
}
void require_inverse_pair(const Matrix& view, const Matrix& inverse) {
    if (!is_rigid(view) || !is_rigid(inverse)) throw std::invalid_argument("X4 camera is not rigid");
    // Componentwise float-error bound, not a large absolute tolerance. This
    // accommodates translation cancellation while still rejecting bad rotations.
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        double value = 0, magnitude = 0;
        for (int k = 0; k < 4; ++k) {
            const double term = double(view.m[r][k])*inverse.m[k][c];
            value += term; magnitude += std::abs(term);
        }
        if (std::abs(value-(r == c ? 1.0 : 0.0)) > 1e-5 + 8*0x1p-23*magnitude)
            throw std::invalid_argument("X4 view and inverse do not correspond");
    }
}
X4Plane visibility_plane(const Matrix& view, double x, double y, double z, double w) {
    double result[4]{};
    const double local[]{x, y, z, w};
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        result[c] += local[r]*view.m[r][c];
    const double length = std::sqrt(result[0]*result[0]+result[1]*result[1]+result[2]*result[2]);
    if (!(std::isfinite(length) && length > 0)) throw std::invalid_argument("Invalid visibility plane");
    return {result[0]/length, result[1]/length, result[2]/length, result[3]/length};
}
}
X4EyeCamera make_x4_eye_camera(std::span<const std::byte> bytes,
                              const EyeView& eye, float scale) {
    if (bytes.size() < 0xcc8) throw std::invalid_argument("Short X4 camera record");
    if (!(std::isfinite(scale) && scale > 0)) throw std::invalid_argument("Invalid X4 units per metre");
    const auto base_inverse = load(bytes, 0);
    const auto base_view = load(bytes, 0x40);
    const auto base_projection = load(bytes, 0x140);
    const auto jitter = load(bytes, 0x1c0);
    const double near_clip = scalar(bytes, 0xcc0), far_clip = scalar(bytes, 0xcc4);
    if (!(near_clip > 0 && far_clip > near_clip) || !near(static_cast<float>(near_clip), base_projection.m[2][3]))
        throw std::invalid_argument("Unsupported X4 clipping planes");
    require_inverse_pair(base_view, base_inverse);
    require_projection(base_projection, true);
    require_projection(eye.projection, false);
    if (!is_rigid(eye.seated_from_eye)) throw std::invalid_argument("Invalid tracked eye pose");
    // Current observed jitter is a clip-space translation, not a camera rotation.
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        if (!(c == 3 && r < 2) && !near(jitter.m[r][c], r == c ? 1.f : 0.f))
            throw std::invalid_argument("Unsupported X4 jitter transform");
    auto basis = Matrix::identity(); basis.m[2][2] = -1;
    auto local = product(product(basis, eye.seated_from_eye), basis);
    for (int r = 0; r < 3; ++r) local.m[r][3] *= scale;
    X4EyeCamera result;
    result.inverse_view = product(base_inverse, local);
    // Compose the captured base view rather than rebuilding its large translation.
    result.view = product(inverse_rigid(local), base_view);
    // Retain runtime asymmetry and cant (cant is in eye pose), but use X4's
    // measured depth convention and near plane, not the runtime frame's far clip.
    result.projection_native.m[0][0] = eye.projection.m[0][0];
    result.projection_native.m[0][2] = -eye.projection.m[0][2];
    result.projection_native.m[1][1] = -eye.projection.m[1][1];
    result.projection_native.m[1][2] = eye.projection.m[1][2];
    result.projection_native.m[2][3] = base_projection.m[2][3];
    result.projection_native.m[3][2] = 1;
    auto flip_y = Matrix::identity(); flip_y.m[1][1] = -1;
    result.projection_vulkan = product(product(flip_y, jitter), result.projection_native);
    result.view_projection = product(result.projection_vulkan, result.view);
    result.projection_culling_native = result.projection_native;
    result.projection_culling_native.m[2][2] = static_cast<float>(far_clip/(far_clip-near_clip));
    result.projection_culling_native.m[2][3] = static_cast<float>(-far_clip*near_clip/(far_clip-near_clip));
    result.culling_view_projection = product(result.projection_culling_native, result.view);
    // Do not extract far distance from rounded projection coefficients: n=0.1,
    // f=400000 is ill-conditioned in float. Use the original scalar clip range.
    const auto& p = result.projection_native;
    const double jitter_x = std::abs(jitter.m[0][3]), jitter_y = std::abs(jitter.m[1][3]);
    result.visibility_planes = {
        visibility_plane(result.view, p.m[0][0], 0, 1+p.m[0][2]+jitter_x, 0),
        visibility_plane(result.view, -p.m[0][0], 0, 1-p.m[0][2]+jitter_x, 0),
        visibility_plane(result.view, 0, p.m[1][1], 1+p.m[1][2]+jitter_y, 0),
        visibility_plane(result.view, 0, -p.m[1][1], 1-p.m[1][2]+jitter_y, 0),
        visibility_plane(result.view, 0, 0, 1, -near_clip),
        visibility_plane(result.view, 0, 0, -1, far_clip)
    };
    return result;
}
bool x4_stereo_sphere_visible(const std::array<X4EyeCamera, 2>& eyes,
                              double x, double y, double z, double radius) {
    if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
          std::isfinite(radius) && radius >= 0)) return true;
    for (const auto& eye : eyes) {
        bool outside = false;
        for (const auto& p : eye.visibility_planes) {
            // An invalid plane must never silently remove geometry.
            if (!(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.w)))
                return true;
            const double distance = p.x*x+p.y*y+p.z*z+p.w;
            // Tangent objects remain visible; tolerate roundoff at the boundary.
            if (distance < -radius-1e-6) { outside = true; break; }
        }
        if (!outside) return true;
    }
    return false;
}
}
