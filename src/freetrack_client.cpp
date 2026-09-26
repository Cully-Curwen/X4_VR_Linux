// FreeTrack client DLL that X4 loads through HKCU\Software\FreeTrack\FreeTrackClient\Path.
// Feeds the OpenVR seated head pose into X4's own head-tracker camera path, so culling,
// object transforms and lighting all follow (unlike patching downstream camera copies).
#include <x4vr/runtime_bootstrap.hpp>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
// Writes `patch` into X4's code at rva+at if the bytes at rva are exactly `expected` (X4 9.00);
// true if patched now or earlier. Anything else is left unchanged.
bool patch_code(const char* what, uintptr_t rva, std::vector<unsigned char> expected, size_t at, const std::vector<unsigned char>& patch) {
    auto* site = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr))+rva;
    auto patched = expected;
    std::copy(patch.begin(), patch.end(), patched.begin()+at);
    if (!std::memcmp(site, patched.data(), patched.size())) return true;
    DWORD previous{};
    if (std::memcmp(site, expected.data(), expected.size()) || !VirtualProtect(site+at, patch.size(), PAGE_EXECUTE_READWRITE, &previous)) {
        OutputDebugStringA(("X4VR freetrack: "+std::string(what)+" signature mismatch; left unchanged\n").c_str());
        return false;
    }
    std::memcpy(site+at, patch.data(), patch.size());
    VirtualProtect(site+at, patch.size(), previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site+at, patch.size());
    OutputDebugStringA(("X4VR freetrack: "+std::string(what)+" patched\n").c_str());
    return true;
}
// X4's head-tracker camera bridge zeroes backward head position (z > 0) at
// 0x9fdb4f, so leaning back or the rear eye while looking sideways got pinned,
// distorting parallax. Turn its guarding `jae` into `jmp`.
void unclamp_backward_position() {
    patch_code("backward head-position clamp", 0x9fdb39, {0x83,0xf8,0x07,0x74,0x15,0xf3,0x0f,0x10,0x45,0x67,0x0f,0x57,
        0x05,0x26,0x4f,0x2c,0x02,0x0f,0x2f,0xc6,0x73,0x04,0x44,0x89,0x65,0x67}, 20, {0xeb});
}
// Head tracking on foot. X4 blocks it twice: the tracker bridge (0x9fd870) sends a zero pose
// while the player has no ship ([player+0x6ab8] invalid) unless the tracker is an eye tracker,
// and Camera::GetOffset (0x97a300) composes the head offset (Camera+0x590) only through a
// camera movement controller (Camera+0x20), which on foot is null. Jump past the zeroing, and
// let a controller-less camera still apply its offset (0x97a5a8 instead of the exit 0x97a694).
bool enable_on_foot_tracking() {
    const bool bridge = patch_code("on-foot head-pose zeroing", 0x9fd9ae, {0x48,0x8b,0x0d,0x43,0xbf,0x31,0x03,0x48,0x85,0xc9,
        0x74,0x19,0x48,0x8b,0x81,0xd0,0x03,0x00,0x00,0x48,0x85,0xc0,0x74,0x0d,0x44,0x39,0xa0,0x68,0x08,0x00,0x00,0x0f,0x85,
        0xbb,0x00,0x00,0x00,0xf7,0x05,0x1f,0x20,0x0e,0x06,0x00,0x04,0x00,0x00,0x0f,0x85,0xab,0x00,0x00,0x00,0x48,0x85,0xc9,
        0x48,0x8d,0x81,0x30,0x02,0x00,0x00,0x75,0x07,0x48,0x8d,0x05,0xe2,0xbc,0x31,0x03,0x48,0x8b,0x10,0x48,0x85,0xd2,0x0f,
        0x84,0x8c,0x00,0x00,0x00,0x8b,0x05,0xc8,0x39,0x31,0x06,0x39,0x82,0xb8,0x6a,0x00,0x00,0x0f,0x94,0xc0,0x84,0xc0,0x74,
        0x79}, 101, {0xeb});
    return bridge && patch_code("on-foot camera offset", 0x97a413, {0x48,0x83,0x7e,0x20,0x00,0x0f,0x84,0x76,0x02,0x00,0x00,
        0x48,0x8b,0x0d,0xd3,0xf4,0x39,0x03,0x48,0x8d,0x91,0x30,0x02,0x00,0x00,0x48,0x85,0xc9,0x75,0x07,0x48,0x8d,0x15,0xa0,
        0xf2,0x39,0x03,0x48,0x8b,0x86,0x70,0x07,0x00,0x00,0x48,0x39,0x02,0x74,0x15,0x48,0x85,0xc9,0x74,0x09,0x48,0x8b,0x81,
        0xd0,0x03,0x00,0x00}, 7, {0x8a,0x01,0x00,0x00});
}
// The rendered camera ([[0x3d198f8]+0x3d0], the global both signatures above reference) in
// mode 0 (+0x868) without a movement controller (+0x20): the player walking.
bool camera_on_foot() {
    const auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto manager = *reinterpret_cast<const unsigned char* const*>(base+0x3d198f8);
    const auto camera = manager ? *reinterpret_cast<const unsigned char* const*>(manager+0x3d0) : nullptr;
    return camera && !*reinterpret_cast<const void* const*>(camera+0x20) && !*reinterpret_cast<const int32_t*>(camera+0x868);
}
bool on_foot_tracking = false;
// Tunables via environment (sign/scale calibration without rebuilding).
float setting(const char* name, float fallback) {
    char text[64]{};
    return GetEnvironmentVariableA(name, text, sizeof(text)) ? static_cast<float>(std::atof(text)) : fallback;
}
// X4's exported UI queries (its Lua API). FTGetData runs on the game thread, so they are
// safe to call here. Each returns false if the export is missing.
template<class F> F game_export(const char* name) { return reinterpret_cast<F>(GetProcAddress(GetModuleHandleW(nullptr), name)); }
bool fullscreen_menu() { // X4 9.00: first argument true = any fullscreen menu, name ignored
    static const auto query = game_export<bool (*)(bool, const char*)>("IsFullscreenMenuDisplayed");
    return query && query(true, nullptr);
}
bool game_flag(const char* name) {
    const auto query = game_export<bool (*)()>(name);
    return query && query();
}
// Ctrl+`key` went down since the last call. X4 binds F11/F12 only without Ctrl (debug keys).
bool ctrl_pressed(int key, bool& was_down) {
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(key) & 0x8000);
    const bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}
bool at_ship_controls() {
    static const auto query = game_export<bool (*)()>("IsPlayerControllingShip");
    return !query || query();
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
            on_foot_tracking = enable_on_foot_tracking();
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
        // On foot the camera takes the head pose one game frame later than in the cockpit
        // (measured: every alternating synthetic axis arrived in the other eye), so this pose
        // is for the next frame's eye, one frame further ahead.
        // ponytail: one frame = 1/90 s (Aero), read the headset refresh if other rates matter.
        const bool walking = on_foot_tracking && camera_on_foot();
        auto head = x4vr::Matrix::identity();
        const bool tracked = runtime->predicted_tracking(head, settings.predict+(walking ? 1.f/90 : 0.f)) == x4vr::FrameStatus::ready;
        if (!tracked && !settings.synth) return FALSE;
        if (!tracked) head = x4vr::Matrix::identity();
        const auto eye = x4vr::render_eye()^uint32_t(walking); // one game frame renders one eye
        // Ctrl+F11 toggles the theater screen by hand: the way out of anything VR gets wrong.
        static bool theater_keys = false, forced_theater = false;
        if (ctrl_pressed(VK_F11, theater_keys)) {
            forced_theater = !forced_theater;
            OutputDebugStringA(forced_theater ? "X4VR freetrack: theater forced on (Ctrl+F11)\n" : "X4VR freetrack: theater forced off (Ctrl+F11)\n");
        }
        // A fullscreen menu (map, inventory, ...) goes to the theater screen, and so does any other
        // view without ship controls or head tracking (cutscenes, walking if the patches failed).
        const bool flat = forced_theater || settings.theater == 2 ||
                          (settings.theater == 1 && (fullscreen_menu() || !(walking || at_ship_controls())));
        if (!settings.synth || flat) x4vr::record_render_pose(head, eye, flat, walking); // reprojection pose (tracking space)
        // Recentre on Ctrl+F12 or when stereo.txt's counter changes.
        static bool recenter_keys = false;
        if (recentered != settings.recenter || ctrl_pressed(VK_F12, recenter_keys)) {
            recentered = settings.recenter;
            const auto origin = x4vr::seated_origin(head);
            origin_inverse = x4vr::inverse_rigid(origin);
            x4vr::publish_view_origin(origin); // the theater screen is placed in front of it
            OutputDebugStringA("X4VR freetrack: head position/yaw recentred\n");
        }
        head = x4vr::multiply(origin_inverse, head);
        if (flat) head = x4vr::Matrix::identity(); // steady, centred view for the virtual screen
        else if (settings.synth) {
            const float sign = eye ? 1.f : -1.f;
            float v[6];
            for (int i = 0; i < 6; ++i) v[i] = settings.synth_base[i] + sign*settings.synth_alt[i];
            // ramp keyed to the frame tag: every call within one game frame agrees
            static float ramp_rate = 0; static uint64_t ramp_start = 0;
            if (settings.synth_rate != ramp_rate) { ramp_rate = settings.synth_rate; ramp_start = x4vr::frame_tag(); }
            v[3] += static_cast<float>(ramp_rate*double(x4vr::frame_tag()-ramp_start));
            head = synthetic_pose(v);
            x4vr::record_render_pose(head, eye, false, walking); // calibration: dumps carry the synthetic pose
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
                // Game state trace, one line per change. Only exports that null-check their game
                // objects (IsHUDActive crashes at the main menu).
                char state[128];
                std::snprintf(state, sizeof(state), "flat=%d menu=%d headtracking=%d ship=%d cutscene=%d walking=%d", flat, fullscreen_menu(),
                              game_flag("IsHeadTrackingActive"), game_flag("IsPlayerControllingShip"), game_flag("IsFullscreenCutsceneActive"), walking);
                static std::string last;
                if (last != state && !fopen_s(&file, (std::string(root)+"/state.txt").c_str(), "a") && file) {
                    last = state;
                    std::fprintf(file, "%llu %s\n", reported, state);
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
