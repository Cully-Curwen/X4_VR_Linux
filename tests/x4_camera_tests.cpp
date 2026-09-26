#include <x4vr/x4_camera.hpp>
#include <x4vr/math.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using x4vr::Matrix;
void require(bool ok) { if (!ok) throw std::runtime_error("X4 eye-camera check failed"); }
void near(float a, float b, float tolerance = 1e-5f) { require(std::isfinite(a) && std::abs(a-b) < tolerance); }
template<class F> void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}
void store(std::span<std::byte> bytes, size_t offset, const Matrix& matrix) {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        std::memcpy(bytes.data()+offset+(c*4+r)*sizeof(float), &matrix.m[r][c], sizeof(float));
}
Matrix load(std::span<const std::byte> bytes, size_t offset) {
    Matrix result;
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        std::memcpy(&result.m[r][c], bytes.data()+offset+(c*4+r)*sizeof(float), sizeof(float));
    return result;
}
std::array<std::byte, 0xd10> fixture() {
    std::array<std::byte, 0xd10> bytes{};
    auto identity = Matrix::identity();
    store(bytes, 0, identity); store(bytes, 0x40, identity); store(bytes, 0x1c0, identity);
    Matrix projection;
    projection.m[0][0] = 1; projection.m[1][1] = 1;
    projection.m[2][3] = 0.1f; projection.m[3][2] = 1;
    store(bytes, 0x140, projection);
    const float clip[]{0.1f, 400000.f};
    std::memcpy(bytes.data()+0xcc0, clip, sizeof(clip));
    return bytes;
}
x4vr::EyeView eye(float x) {
    x4vr::EyeView result;
    result.seated_from_eye = Matrix::identity(); result.seated_from_eye.m[0][3] = x;
    result.projection = x4vr::vulkan_projection(-1.2f, 0.8f, -0.9f, 1.1f, 0.1f, 1000);
    return result;
}
// Turn compensation: yaw about +Y, and OpenVR <-> X4 camera axes (z back <-> z forward).
Matrix yawed(float degrees, float x = 0) {
    auto r = Matrix::identity();
    const float a = degrees*3.14159265f/180;
    r.m[0][0] = r.m[2][2] = std::cos(a); r.m[0][2] = std::sin(a); r.m[2][0] = -std::sin(a); r.m[0][3] = x;
    return r;
}
Matrix flip_z(Matrix m) { for (int i = 0; i < 2; ++i) { m.m[i][2] = -m.m[i][2]; m.m[2][i] = -m.m[2][i]; } return m; }
// X4 view matrix for body rotation `body` (X4 axes) and eye pose `eye` (OpenVR tracking space).
Matrix x4_view(const Matrix& body, const Matrix& eye) {
    auto camera = x4vr::multiply(body, flip_z(eye)); camera.m[0][3] = camera.m[1][3] = camera.m[2][3] = 0;
    auto view = Matrix::identity();
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) view.m[i][j] = camera.m[j][i];
    view.m[0][3] = 7; // translation plays no part
    return view;
}
void turn_compensation_checks() {
    const auto left = yawed(0, -0.032f), right = yawed(0, 0.032f);
    const auto head_l = yawed(4, 0.1f), head_r = yawed(-6, 0.2f); // head turned between the frames
    const auto eye_l = x4vr::multiply(head_l, left), eye_r = x4vr::multiply(head_r, right);
    // Head motion only: each image keeps its own pose.
    auto same = x4vr::turned_pose(head_r, right, x4_view(yawed(30), eye_r), head_l, left, x4_view(yawed(30), eye_l));
    require(x4vr::rotation_degrees(same, head_l) < 0.05); // float rounding near acos(1)
    near(same.m[0][3], 0.1f);
    // Body turned 5 degrees between the frames: the older image is shown 5 degrees turned.
    auto turned = x4vr::turned_pose(head_r, right, x4_view(yawed(30), eye_r), head_l, left, x4_view(yawed(25), eye_l));
    const auto expected = x4vr::multiply(flip_z(yawed(-5)), head_l);
    require(x4vr::rotation_degrees(turned, expected) < 0.05 && std::abs(x4vr::rotation_degrees(turned, head_l)-5) < 0.05);
}
int main(int argc, char** argv) {
    try {
        turn_compensation_checks();
        auto bytes = fixture();
        const auto unchanged = bytes;
        const auto left = x4vr::make_x4_eye_camera(bytes, eye(-0.032f), 1);
        const auto right = x4vr::make_x4_eye_camera(bytes, eye(0.032f), 1);
        require(bytes == unchanged);
        near(left.inverse_view.m[0][3], -0.032f); near(left.view.m[0][3], 0.032f);
        near(right.inverse_view.m[0][3]-left.inverse_view.m[0][3], 0.064f);
        const auto scaled = x4vr::make_x4_eye_camera(bytes, eye(0.032f), 10);
        near(scaled.inverse_view.m[0][3], 0.32f);
        auto moved = eye(0);
        moved.seated_from_eye.m[1][3] = 0.2f; moved.seated_from_eye.m[2][3] = -0.3f;
        const auto translated = x4vr::make_x4_eye_camera(bytes, moved, 1);
        near(translated.inverse_view.m[1][3], 0.2f); near(translated.inverse_view.m[2][3], 0.3f);
        moved.seated_from_eye = Matrix::identity();
        const float angle = 0.2f;
        moved.seated_from_eye.m[0][0] = moved.seated_from_eye.m[2][2] = std::cos(angle);
        moved.seated_from_eye.m[0][2] = std::sin(angle); moved.seated_from_eye.m[2][0] = -std::sin(angle);
        const auto canted = x4vr::make_x4_eye_camera(bytes, moved, 1);
        near(canted.inverse_view.m[0][2], -std::sin(angle));
        near(canted.inverse_view.m[2][0], std::sin(angle));
        const auto& p = left.projection_vulkan;
        near(p.m[2][2], 0); near(p.m[2][3]/0.1f, 1); near(p.m[2][3]/100000.f, 0, 2e-6f);
        const std::array stereo{left, right};
        require(x4vr::x4_stereo_sphere_visible(stereo, 0, 0, 1, 0));
        require(!x4vr::x4_stereo_sphere_visible(stereo, 0, 0, -1, 0));
        require(!x4vr::x4_stereo_sphere_visible(stereo, 0, 0, 0.05, 0));
        require(x4vr::x4_stereo_sphere_visible(stereo, 0, 0, 0.05, 0.05));
        require(x4vr::x4_stereo_sphere_visible(stereo, 0, 0, 399999, 0));
        require(!x4vr::x4_stereo_sphere_visible(stereo, 0, 0, 400001, 0));
        require(x4vr::x4_stereo_sphere_visible(stereo, 0, 0, 400001, 2));
        // At z=1, right edge is x=0.8 relative to each eye. This point is
        // outside the left eye but inside the right eye: intersection is wrong.
        require(x4vr::x4_stereo_sphere_visible(stereo, 0.8, 0, 1, 0));
        require(!x4vr::x4_stereo_sphere_visible(std::array{left, left}, 0.8, 0, 1, 0));
        require(!x4vr::x4_stereo_sphere_visible(stereo, 1, 0, 1, 0));
        require(x4vr::x4_stereo_sphere_visible(stereo, 0, 0, 1, -1));
        const auto runtime = eye(0).projection;
        for (int row = 0; row < 2; ++row) {
            near(p.m[row][row], runtime.m[row][row]);
            near(p.m[row][2], -runtime.m[row][2]);
        }
        auto jitter = Matrix::identity(); jitter.m[0][3] = 0.01f; jitter.m[1][3] = 0.02f;
        store(bytes, 0x1c0, jitter);
        const auto jittered = x4vr::make_x4_eye_camera(bytes, eye(0), 1);
        near(jittered.projection_vulkan.m[0][2]-p.m[0][2], 0.01f);
        near(jittered.projection_vulkan.m[1][2]-p.m[1][2], -0.02f);
        require(x4vr::x4_stereo_sphere_visible(std::array{jittered,jittered}, 0.805, 0, 1, 0));
        rejects([&] { x4vr::make_x4_eye_camera({}, eye(0), 1); });
        rejects([&] { x4vr::make_x4_eye_camera(bytes, eye(0), 0); });
        rejects([&] { x4vr::make_x4_eye_camera(bytes, eye(0), std::numeric_limits<float>::infinity()); });
        auto invalid = eye(0); invalid.seated_from_eye.m[0][0] = 2;
        rejects([&] { x4vr::make_x4_eye_camera(bytes, invalid, 1); });
        auto bad = Matrix::identity(); store(bytes, 0x140, bad);
        rejects([&] { x4vr::make_x4_eye_camera(bytes, eye(0), 1); });
        bytes = fixture(); bad.m[0][3] = 1; store(bytes, 0x40, bad);
        rejects([&] { x4vr::make_x4_eye_camera(bytes, eye(0), 1); });
        for (int i = 1; i < argc; ++i) {
            std::ifstream file(argv[i], std::ios::binary);
            std::array<std::byte, 0xd10> captured{};
            if (!file.read(reinterpret_cast<char*>(captured.data()), captured.size()))
                throw std::runtime_error("Cannot read captured native camera");
            const auto a = x4vr::make_x4_eye_camera(captured, eye(-0.032f), 1);
            const auto b = x4vr::make_x4_eye_camera(captured, eye(0.032f), 1);
            double distance = 0;
            for (int r = 0; r < 3; ++r) distance += std::pow(double(a.inverse_view.m[r][3])-b.inverse_view.m[r][3], 2);
            near(static_cast<float>(std::sqrt(distance)), 0.064f, 0.005f);
            // Both eye views must remain distinct even at large world coordinates.
            require(std::memcmp(&a.view, &b.view, sizeof(Matrix)) != 0);
            double center[3]{};
            for (int r = 0; r < 3; ++r) center[r] = double(a.inverse_view.m[r][3])+a.inverse_view.m[r][2]*10;
            require(x4vr::x4_stereo_sphere_visible(std::array{a,b}, center[0], center[1], center[2], 1));
            // Neutral eye with the captured optical frustum must reproduce the
            // native finite projection and its view-product at +0xc0 / +0x100.
            auto neutral_eye = eye(0);
            neutral_eye.projection = load(captured, 0x140);
            for (int r = 0; r < 4; ++r) neutral_eye.projection.m[r][2] *= -1;
            for (int c = 0; c < 4; ++c) neutral_eye.projection.m[1][c] *= -1;
            const auto neutral = x4vr::make_x4_eye_camera(captured, neutral_eye, 1);
            const auto finite = load(captured, 0xc0), finite_vp = load(captured, 0x100);
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
                near(neutral.projection_culling_native.m[r][c], finite.m[r][c]);
                near(neutral.culling_view_projection.m[r][c], finite_vp.m[r][c],
                     1e-5f + 1e-6f*std::abs(finite_vp.m[r][c]));
            }
            std::cout << "Captured camera eye composition passed: " << argv[i] << '\n';
        }
        std::cout << "X4 eye composition, cant, scale, projection, jitter and rejection checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
