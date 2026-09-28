// FreeTrack client DLL that X4 loads through HKCU\Software\FreeTrack\FreeTrackClient\Path.
// Feeds the OpenVR seated head pose into X4's own head-tracker camera path, so culling,
// object transforms and lighting all follow (unlike patching downstream camera copies).
#include <x4vr/runtime_bootstrap.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
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
// Eye at use (2026-09-28). X4 builds a frame's camera from whichever tracker read came last, and a
// read that lands unusually early or late in X4's frame is used one frame sooner or later: with
// the eye chosen here, ~2% of frames at 90 fps (up to 38% unpaced) carried the other eye's offset
// and jumped sideways in the headset. So FTGetData sends the head centre, and the camera bridge's
// read of the tracker position (vtable slot 0x108, X4 9.00: 0xf377b0) adds the offset of the eye
// X4 builds at that moment, from the frame half then.
struct EyeOffsets {
    bool valid = false; // the last read sent the centre; add these
    bool walking = false; // on foot the camera applies the pose one frame later
    std::array<std::array<float, 3>, 2> delta{}; // per eye, FreeTrack position units
    std::array<x4vr::Matrix, 2> head{}; // reprojection pose per eye (tracking space)
};
EyeOffsets eye_offsets; // written by FTGetData (X4's main thread), read on X4's camera thread
std::mutex offsets_mutex;
using TrackerPosition = void (*)(void* tracker, float* x, float* y, float* z);
TrackerPosition tracker_position{};
void position_at_use(void* tracker, float* x, float* y, float* z) {
    EyeOffsets offsets;
    { std::lock_guard lock(offsets_mutex); offsets = eye_offsets; }
    if (!offsets.valid) return tracker_position(tracker, x, y, z);
    const auto settings = x4vr::stereo_settings();
    const auto eye = x4vr::render_eye()^uint32_t(offsets.walking ? settings.half_xor_walk : settings.half_xor_use); // the frame X4 builds now
    // X4 9.00: position = FreeTrack xyz * tracker[+0x104] * 0.2 ([0x2cbe324]), smoothed into +0xc0
    // (strength 1: the last read); the accessor scales and clamps it. Offset it for this call only.
    static const float gain = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(GetModuleHandleW(nullptr))+0x2cbe324);
    auto* position = reinterpret_cast<float*>(static_cast<char*>(tracker)+0xc0);
    const float scale = *reinterpret_cast<const float*>(static_cast<char*>(tracker)+0x104)*gain;
    const std::array<float, 3> centre{position[0], position[1], position[2]};
    for (int i = 0; i < 3; ++i) position[i] += offsets.delta[eye][i]*scale;
    tracker_position(tracker, x, y, z);
    for (int i = 0; i < 3; ++i) position[i] = centre[i];
    x4vr::record_render_pose(offsets.head[eye], eye, false, offsets.walking);
    x4vr::trace_event(eye ? 'r' : 'l', x4vr::frame_tag());
}
// X4 stops reading the pose (the bridge skips both accessors) once 30 reads in a row barely
// changed (vtable slot 0x28, 0xf376c0). The alternating eye offsets used to prevent that; the
// centre alone can be that still, so the tracker never counts as still while offsets are added.
using TrackerStill = bool (*)(void* tracker);
TrackerStill tracker_still{};
bool still_at_use(void* tracker) {
    { std::lock_guard lock(offsets_mutex); if (eye_offsets.valid) return false; }
    return tracker_still(tracker);
}
// Replaces vtable[offset] if it holds base+rva whose code starts with `code`.
template<size_t N> bool swap_slot(void** vtable, size_t offset, const unsigned char* base, uintptr_t rva, const unsigned char (&code)[N],
                                  void* replacement, void** original) {
    auto** slot = vtable+offset/sizeof(void*);
    DWORD previous{};
    if (*slot != base+rva || std::memcmp(base+rva, code, N) || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    *original = *slot;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), previous, &previous);
    return true;
}
// Installs position_at_use and still_at_use once, if the tracker around `data` is X4 9.00's FreeTrack tracker.
bool eye_hook_ready(FreeTrackData* data) {
    static int state = 0; // 0 untried, 1 installed, -1 refused
    if (state) return state > 0;
    state = -1;
    const auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    auto* tracker = reinterpret_cast<unsigned char*>(data)-0x30; // X4 passes tracker+0x30 (0xf37485)
    static constexpr unsigned char accessor[] = {0x0f,0x10,0x81,0xc0,0x00,0x00,0x00,0x48,0x8d,0x44,0x24,0x08,0xf3,0x0f,0x10,0x15,
        0xb4,0x69,0xd8,0x01,0xf3,0x0f,0x10,0x1d,0x08,0x6e,0xd8,0x01,0xf3,0x0f,0x59,0xc2};
    static constexpr unsigned char update[] = {0xf3,0x0f,0x10,0xa3,0x04,0x01,0x00,0x00,0x0f,0x28,0xd4,0x0f,0x28,0xdc,0xf3,0x0f,
        0x59,0x63,0x48,0xf3,0x0f,0x59,0x5b,0x4c,0xf3,0x0f,0x59,0x53,0x50,0xf3,0x0f,0x59,0xe0,0xf3,0x0f,0x59,0xd8,0xf3,0x0f,0x59,
        0xd0,0x0f,0x28,0xec,0x0f,0x14,0xeb,0x0f,0x16,0xea,0x0f,0x11,0xab,0xa0,0x00,0x00,0x00};
    static constexpr unsigned char still[] = {0x83,0xb9,0x9c,0x00,0x00,0x00,0x1e,0x0f,0x93,0xc0,0xc3}; // cmp [rcx+9c],30; setae al
    auto** vtable = *reinterpret_cast<void***>(tracker);
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&eye_offsets), &self);
    void* original_still{};
    if (*reinterpret_cast<FARPROC*>(tracker+0x10) != GetProcAddress(self, "FTGetData") || std::memcmp(base+0xf37595, update, sizeof(update)) ||
        vtable[0x108/sizeof(void*)] != base+0xf377b0 || std::memcmp(base+0xf377b0, accessor, sizeof(accessor)) ||
        !swap_slot(vtable, 0x28, base, 0xf376c0, still, reinterpret_cast<void*>(&still_at_use), &original_still)) {
        OutputDebugStringA("X4VR freetrack: eye-at-use hook signature mismatch; eyes chosen at the tracker read\n");
        return false;
    }
    tracker_still = reinterpret_cast<TrackerStill>(original_still);
    void* original_position{};
    if (!swap_slot(vtable, 0x108, base, 0xf377b0, accessor, reinterpret_cast<void*>(&position_at_use), &original_position)) {
        OutputDebugStringA("X4VR freetrack: eye-at-use position hook failed; eyes chosen at the tracker read\n");
        return false; // still_at_use passes through while no offsets are added
    }
    tracker_position = reinterpret_cast<TrackerPosition>(original_position);
    OutputDebugStringA("X4VR freetrack: eye-at-use hook installed\n");
    state = 1;
    return true;
}
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
        x4vr::trace_event(eye ? 'R' : 'L', x4vr::frame_tag());
        static bool caller_logged = false;
        if (!caller_logged) { // diagnostics: where X4 reads the tracker (RVAs in X4.exe) and into what buffer
            caller_logged = true;
            void* frames[16]{};
            const auto count = CaptureStackBackTrace(0, 16, frames, nullptr);
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            char line[512];
            int used = std::snprintf(line, sizeof(line), "X4VR freetrack: FTGetData thread %lu data %p exe %p callers", GetCurrentThreadId(),
                                     static_cast<void*>(data), reinterpret_cast<void*>(base));
            for (USHORT i = 0; i < count && used > 0 && used < int(sizeof(line))-24; ++i) {
                const auto at = reinterpret_cast<uintptr_t>(frames[i]);
                if (at >= base && at-base < 0x10000000) used += std::snprintf(line+used, sizeof(line)-size_t(used), " %#llx", static_cast<unsigned long long>(at-base));
            }
            OutputDebugStringA((std::string(line)+"\n").c_str());
            char root[1024]{};
            if (GetEnvironmentVariableA("X4VR_CAPTURE_DIR", root, sizeof(root)))
                x4vr::write_file_later(std::string(root)+"/ftgetdata_caller.txt", std::string(line)+"\n", false);
        }
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
        const auto tracking_head = head; // reprojection pose (tracking space)
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
        // head: the pose sent to X4; eye_head: each eye's; recorded: each eye's reprojection pose.
        std::array<x4vr::Matrix, 2> eye_head{head, head}, recorded{tracking_head, tracking_head};
        bool alternate = false; // the eyes differ in position only, so the offset can be added at use
        if (flat) head = eye_head[0] = eye_head[1] = x4vr::Matrix::identity(); // steady, centred view for the virtual screen
        else if (settings.synth) { // calibration: dumps carry the synthetic pose
            // ramp keyed to the frame tag: every call within one game frame agrees
            static float ramp_rate = 0; static uint64_t ramp_start = 0;
            if (settings.synth_rate != ramp_rate) { ramp_rate = settings.synth_rate; ramp_start = x4vr::frame_tag(); }
            const auto ramp = static_cast<float>(ramp_rate*double(x4vr::frame_tag()-ramp_start));
            float v[6];
            for (int side = -1; side <= 1; ++side) { // -1 left, 0 centre, 1 right
                for (int i = 0; i < 6; ++i) v[i] = settings.synth_base[i] + float(side)*settings.synth_alt[i];
                v[3] += ramp;
                (side ? eye_head[side > 0] : head) = synthetic_pose(v);
            }
            recorded = eye_head;
            alternate = !settings.synth_alt[3] && !settings.synth_alt[4] && !settings.synth_alt[5];
        } else if (settings.stereo) {
            // Alternate-eye rendering: this game frame renders one eye; the Vulkan
            // layer submits the presented image to the same eye (shared counter).
            // Read on first use: a tracked pose here means the runtime's session runs (OpenXR has
            // no eye setup before that).
            static const auto eyes = runtime->eye_setup();
            for (uint32_t e = 0; e < 2; ++e) {
                auto head_from_eye = eyes.head_from_eye[e];
                for (int r = 0; r < 3; ++r) head_from_eye.m[r][3] *= settings.ipd_scale;
                eye_head[e] = x4vr::multiply(head, head_from_eye);
            }
            alternate = true;
        }
        // On foot Camera::GetOffset applies what the bridge read one frame later (walk_at_use, half_xor_walk).
        const bool at_use = alternate && (!walking || settings.walk_at_use) && settings.eye_at_use && eye_hook_ready(data);
        if (!at_use) {
            head = eye_head[eye];
            if (!settings.synth || flat) x4vr::record_render_pose(tracking_head, eye, flat, walking);
            else x4vr::record_render_pose(recorded[eye], eye, false, walking);
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
        {
            std::lock_guard offsets_lock(offsets_mutex);
            eye_offsets.valid = at_use;
            eye_offsets.walking = walking;
            if (at_use) {
                const float axis[3]{sx, sh, sz};
                for (uint32_t e = 0; e < 2; ++e)
                    for (int k = 0; k < 3; ++k) eye_offsets.delta[e][k] = settings.pos_scale*axis[k]*(eye_head[e].m[k][3]-m[k][3]);
                eye_offsets.head = recorded;
            }
        }
        if (GetTickCount64()-reported > 200) { // calibration trace: pose actually sent (pre-gain)
            reported = GetTickCount64();
            char root[1024]{};
            if (GetEnvironmentVariableA("X4VR_CAPTURE_DIR", root, sizeof(root))) { // written off X4's game thread
                char line[256];
                std::snprintf(line, sizeof(line), "%llu %f %f %f %f %f %f\n", reported, yaw, pitch, roll, m[0][3], m[1][3], m[2][3]);
                x4vr::write_file_later(std::string(root)+"/head.txt", line, false);
                // Game state trace, one line per change. Only exports that null-check their game
                // objects (IsHUDActive crashes at the main menu).
                char state[128];
                std::snprintf(state, sizeof(state), "flat=%d menu=%d headtracking=%d ship=%d cutscene=%d walking=%d", flat, fullscreen_menu(),
                              game_flag("IsHeadTrackingActive"), game_flag("IsPlayerControllingShip"), game_flag("IsFullscreenCutsceneActive"), walking);
                static std::string last;
                if (last != state) {
                    last = state;
                    x4vr::write_file_later(std::string(root)+"/state.txt", std::to_string(reported)+' '+state+'\n', true);
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
