#include <x4vr/head_look.hpp>
#include <x4vr/pose_control.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace x4vr;
using namespace x4vr::experimental;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
Matrix yaw(float degrees) {
    const float a = degrees*3.14159265359f/180;
    auto p = Matrix::identity();
    p.m[0][0] = p.m[2][2] = std::cos(a); p.m[0][2] = std::sin(a); p.m[2][0] = -std::sin(a);
    return p;
}
NativePose native(const Matrix& matrix) {
    NativePose p;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) p[c*4+r] = matrix.m[r][c];
    return p;
}
void expect_near(float a, float b) { check(std::isfinite(a) && std::abs(a-b) < 0.00002f, "Head-look matrix mismatch"); }
void pose_math() {
    auto base_matrix = yaw(17); base_matrix.m[0][3] = 31077.71f; base_matrix.m[1][3] = 383; base_matrix.m[2][3] = -6479.85f;
    const auto base = native(base_matrix);
    NativePose output{};
    HeadLook look;
    check(!look.apply(base, Matrix::identity(), true, output), "Disabled head-look applied");
    check(output == NativePose{}, "Disabled output overwritten");
    look.enable();
    check(!look.apply(base, Matrix::identity(), false, output), "Lost tracking applied");
    // Pending recenter survives lost tracking and invalid native input.
    auto invalid_base = base; invalid_base[0] = 5;
    check(!look.apply(invalid_base, yaw(2), true, output), "Invalid base applied");
    auto origin = yaw(30); origin.m[0][3] = 5; origin.m[1][3] = 1.8f;
    check(look.apply(base, origin, true, output), "Initial recenter failed");
    for (int i = 0; i < 16; ++i) expect_near(output[i], base[i]);
    // OpenVR +65 degree local yaw must become native -65, as observed for left.
    auto head = yaw(95); head.m[0][3] = 200; head.m[1][3] = -50;
    check(look.apply(base, head, true, output), "Valid tracking rejected");
    const auto expected = native(multiply(base_matrix, yaw(-65)));
    for (int i = 0; i < 16; ++i) expect_near(output[i], expected[i]);
    for (int i = 12; i < 16; ++i) check(output[i] == base[i], "World translation changed");
    const auto first = output;
    check(look.apply(base, head, true, output) && output == first, "Pose accumulates each frame");
    head.m[1][1] = std::numeric_limits<float>::quiet_NaN();
    check(!look.apply(base, head, true, output) && output == first, "Invalid tracking wrote output");
    look.recenter();
    check(look.apply(base, yaw(-60), true, output), "Explicit recenter failed");
    for (int i = 0; i < 16; ++i) expect_near(output[i], base[i]);
    look.disable();
    check(!look.apply(base, yaw(0), true, output), "Disable failed");
    look.enable();
    auto pitch = Matrix::identity();
    const float angle = 35*3.14159265359f/180;
    pitch.m[1][1] = pitch.m[2][2] = std::cos(angle); pitch.m[1][2] = -std::sin(angle); pitch.m[2][1] = std::sin(angle);
    check(look.apply(native(Matrix::identity()), pitch, true, output), "Pitch rejected");
    expect_near(output[9], std::sin(angle)); expect_near(output[6], -std::sin(angle)); // native pitch -35 (up)
    // Changing the game base must remain effective, rather than freezing a cached base.
    look.recenter();
    const auto rotated_base = native(yaw(-40));
    check(look.apply(rotated_base, Matrix::identity(), true, output), "Updated game base rejected");
    for (int i = 0; i < 16; ++i) expect_near(output[i], rotated_base[i]);
}
void controls() {
    const auto pid = GetCurrentProcessId();
    const auto signal = [pid](PoseCommand command) {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, pose_event_name(pid, command).c_str());
        check(event != nullptr, "Control missing");
        const bool sent = SetEvent(event) != FALSE;
        CloseHandle(event); check(sent, "Control signal failed");
    };
    {
        PoseControls control(pid);
        check(control.poll() == PoseCommand::none, "Controls start enabled");
        signal(PoseCommand::enable);
        check(control.poll() == PoseCommand::enable && control.poll() == PoseCommand::none, "Enable not consumed once");
        signal(PoseCommand::enable); signal(PoseCommand::disable);
        check(control.poll() == PoseCommand::disable && control.poll() == PoseCommand::none, "Disable precedence failed");
        signal(PoseCommand::recenter);
        check(control.poll() == PoseCommand::recenter, "Recenter failed");
        signal(PoseCommand::enable); signal(PoseCommand::shutdown);
        check(control.poll() == PoseCommand::shutdown && control.poll() == PoseCommand::none, "Shutdown precedence failed");
        bool rejected = false;
        try { PoseControls duplicate(pid); } catch (const std::exception&) { rejected = true; }
        check(rejected, "Existing control names accepted");
        signal(PoseCommand::enable);
        check(control.poll() == PoseCommand::enable, "Duplicate rejection destroyed original controls");
    }
    HANDLE absent = OpenEventW(EVENT_MODIFY_STATE, FALSE, pose_event_name(pid, PoseCommand::enable).c_str());
    if (absent) CloseHandle(absent);
    check(!absent, "Controls leaked");
}
int main() {
    try {
        pose_math(); controls();
        std::cout << "Head-look axis mapping, recenter, tracking loss, translation preservation, non-accumulation and controls passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
