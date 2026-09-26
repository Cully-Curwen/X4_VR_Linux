// FreeTrack client DLL that X4 loads through HKCU\Software\FreeTrack\FreeTrackClient\Path.
// Feeds the OpenVR seated head pose into X4's own head-tracker camera path, so culling,
// object transforms and lighting all follow (unlike patching downstream camera copies).
#include <x4vr/runtime_bootstrap.hpp>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace {
struct FreeTrackData {
    uint32_t data_id;
    int32_t cam_width, cam_height;
    float yaw, pitch, roll, x, y, z;          // radians, millimetres
    float raw_yaw, raw_pitch, raw_roll, raw_x, raw_y, raw_z;
    float x1, y1, x2, y2, x3, y3, x4, y4;
};
static_assert(sizeof(FreeTrackData) == 92);

std::mutex mutex;
std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
std::atomic_uint32_t id{};
bool failed{}, logged{};
// Recentre: X4 clamps normalized head position to +-1 (= 0.25 m), so the seated
// origin must sit at the user's actual head, with yaw facing the cockpit forward.
x4vr::Matrix origin_inverse = x4vr::Matrix::identity();
int recentered = -1;
uint64_t reported{};

// OpenVR convention: +X right, +Y up, -Z forward; yaw about +Y (positive = left),
// pitch about +X (positive = up), roll about -Z... composed as T * Ry * Rx * Rz.
x4vr::Matrix synthetic_pose(const float v[6]) {
    const double d = 3.14159265358979323846/180, y = v[3]*d, p = v[4]*d, r = v[5]*d;
    auto ry = x4vr::Matrix::identity(), rx = ry, rz = ry;
    ry.m[0][0] = float(std::cos(y)); ry.m[0][2] = float(std::sin(y)); ry.m[2][0] = float(-std::sin(y)); ry.m[2][2] = float(std::cos(y));
    rx.m[1][1] = float(std::cos(p)); rx.m[1][2] = float(-std::sin(p)); rx.m[2][1] = float(std::sin(p)); rx.m[2][2] = float(std::cos(p));
    rz.m[0][0] = float(std::cos(r)); rz.m[0][1] = float(-std::sin(r)); rz.m[1][0] = float(std::sin(r)); rz.m[1][1] = float(std::cos(r));
    auto pose = x4vr::multiply(x4vr::multiply(ry, rx), rz);
    pose.m[0][3] = v[0]; pose.m[1][3] = v[1]; pose.m[2][3] = v[2];
    return pose;
}
// X4's head-tracker camera bridge zeroes backward head position (z > 0) at
// 0x9fdb4f, so leaning back or the rear eye while looking sideways got pinned,
// distorting parallax. Turn its guarding `jae` into `jmp` (exact bytes verified).
void unclamp_backward_position() {
    static constexpr unsigned char expected[] = {0x83,0xf8,0x07,0x74,0x15,0xf3,0x0f,0x10,0x45,0x67,0x0f,0x57,
        0x05,0x26,0x4f,0x2c,0x02,0x0f,0x2f,0xc6,0x73,0x04,0x44,0x89,0x65,0x67};
    auto* site = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr))+0x9fdb39;
    if (site[20] == 0xeb) return;
    if (std::memcmp(site, expected, sizeof(expected))) {
        OutputDebugStringA("X4VR freetrack: backward-position clamp signature mismatch; left unchanged\n");
        return;
    }
    DWORD previous{};
    if (!VirtualProtect(site+20, 1, PAGE_EXECUTE_READWRITE, &previous)) return;
    site[20] = 0xeb;
    VirtualProtect(site+20, 1, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site+20, 1);
    OutputDebugStringA("X4VR freetrack: backward head-position clamp disabled\n");
}
// Tunables via environment (sign/scale calibration without rebuilding).
float setting(const char* name, float fallback) {
    char text[64]{};
    return GetEnvironmentVariableA(name, text, sizeof(text)) ? static_cast<float>(std::atof(text)) : fallback;
}
}

extern "C" __declspec(dllexport) BOOL __cdecl FTGetData(FreeTrackData* data) {
    if (!data) return FALSE;
    std::lock_guard lock(mutex);
    if (failed) return FALSE;
    try {
        if (!runtime) {
            runtime = x4vr::acquire_runtime_bootstrap();
            unclamp_backward_position();
            // X4 smooths tracker input with alpha = 1/strength; its menu minimum is 5,
            // which lags rotation and averages alternating eye offsets away. The
            // exported setter accepts 1 (= no smoothing) and persists it.
            const auto game = GetModuleHandleW(nullptr);
            const auto set = reinterpret_cast<void (*)(int64_t)>(GetProcAddress(game, "SetActiveHeadTrackerHeadFilterStrength"));
            const auto get = reinterpret_cast<int64_t (*)()>(GetProcAddress(game, "GetActiveHeadTrackerHeadFilterStrength"));
            if (set && get) {
                set(1);
                OutputDebugStringA(get() == 1 ? "X4VR freetrack: head smoothing disabled (strength 1)\n"
                                              : "X4VR freetrack: head smoothing change rejected\n");
            }
        }
        const auto settings = x4vr::stereo_settings();
        static const auto eyes = runtime->eye_setup();
        auto head = x4vr::Matrix::identity();
        const bool tracked = runtime->predicted_tracking(head, settings.predict) == x4vr::FrameStatus::ready;
        if (!tracked && !settings.synth) return FALSE;
        if (!tracked) head = x4vr::Matrix::identity();
        const auto eye = x4vr::render_eye(); // one game frame renders one eye
        if (!settings.synth) x4vr::record_render_pose(head, eye); // reprojection pose (tracking space)
        // Recentre on Ctrl+F12 (edge-triggered; unbound in X4) or when stereo.txt's counter changes.
        static bool keys_were_down = false;
        const bool keys_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F12) & 0x8000);
        const bool hotkey = keys_down && !keys_were_down;
        keys_were_down = keys_down;
        if (recentered != settings.recenter || hotkey) {
            recentered = settings.recenter;
            origin_inverse = x4vr::inverse_rigid(x4vr::seated_origin(head));
            OutputDebugStringA("X4VR freetrack: head position/yaw recentred\n");
        }
        head = x4vr::multiply(origin_inverse, head);
        if (settings.synth) {
            const float sign = eye ? 1.f : -1.f;
            float v[6];
            for (int i = 0; i < 6; ++i) v[i] = settings.synth_base[i] + sign*settings.synth_alt[i];
            // ramp keyed to the frame tag: every call within one game frame agrees
            static float ramp_rate = 0; static uint64_t ramp_start = 0;
            if (settings.synth_rate != ramp_rate) { ramp_rate = settings.synth_rate; ramp_start = x4vr::frame_tag(); }
            v[3] += static_cast<float>(ramp_rate*double(x4vr::frame_tag()-ramp_start));
            head = synthetic_pose(v);
            x4vr::record_render_pose(head, eye); // calibration: dumps carry the synthetic pose
        } else if (settings.stereo) {
            // Alternate-eye rendering: this game frame renders one eye; the Vulkan
            // layer submits the presented image to the same eye (shared counter).
            x4vr::trace_event(eye ? 'R' : 'L', x4vr::frame_tag());
            auto head_from_eye = eyes.head_from_eye[eye];
            for (int r = 0; r < 3; ++r) head_from_eye.m[r][3] *= settings.ipd_scale;
            head = x4vr::multiply(head, head_from_eye);
        }
        const auto& m = head.m; // row-major, OpenVR seated: +X right, +Y up, -Z forward, metres
        // Y(yaw)-X(pitch)-Z(roll) intrinsic decomposition of R = Ry*Rx*Rz.
        const float yaw = std::atan2(m[0][2], m[2][2]);
        const float pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, -m[1][2])));
        const float roll = std::atan2(m[1][0], m[1][1]);
        static const float sy = setting("X4VR_FT_YAW", 1), sp = setting("X4VR_FT_PITCH", -1), sr = setting("X4VR_FT_ROLL", 1);
        static const float sx = setting("X4VR_FT_X", -1000), sh = setting("X4VR_FT_Y", 1000), sz = setting("X4VR_FT_Z", 1000);
        *data = {};
        data->data_id = ++id;
        // X4 maps angle/pi to +-1 and then multiplies by 85 deg, not 180: undo that gain.
        data->yaw = data->raw_yaw = settings.yaw_gain*sy*yaw;
        data->pitch = data->raw_pitch = settings.pitch_gain*sp*pitch;
        data->roll = data->raw_roll = settings.roll_gain*sr*roll;
        // Calibrated: X4 applies tracker translation in the ship frame (not head-rotated).
        const float local[3]{m[0][3], m[1][3], m[2][3]};
        data->x = data->raw_x = settings.pos_scale*sx*local[0];
        data->y = data->raw_y = settings.pos_scale*sh*local[1];
        data->z = data->raw_z = settings.pos_scale*sz*local[2];
        if (GetTickCount64()-reported > 200) { // calibration trace: pose actually sent (pre-gain)
            reported = GetTickCount64();
            char root[1024]{};
            if (GetEnvironmentVariableA("X4VR_CAPTURE_DIR", root, sizeof(root))) {
                FILE* file{};
                if (!fopen_s(&file, (std::string(root)+"/head.txt").c_str(), "w") && file) {
                    std::fprintf(file, "%llu %f %f %f %f %f %f\n", reported, yaw, pitch, roll, m[0][3], m[1][3], m[2][3]);
                    std::fclose(file);
                }
            }
        }
        if (!logged) { logged = true; OutputDebugStringA("X4VR freetrack: first headset pose delivered to X4\n"); }
        return TRUE;
    } catch (const std::exception& error) {
        failed = true;
        OutputDebugStringA("X4VR freetrack: runtime failure; tracking disabled\n");
        OutputDebugStringA(error.what());
        return FALSE;
    }
}
extern "C" __declspec(dllexport) const char* __cdecl FTGetDllVersion() { return "2.0.0.0"; }
extern "C" __declspec(dllexport) const char* __cdecl FTProvider() { return "X4VR OpenVR"; }
extern "C" __declspec(dllexport) void __cdecl FTReportName(int) {}
