#pragma once
#include <x4vr/session.hpp>
#include <memory>
#include <mutex>

namespace x4vr {
// Initialization and unpaced tracking bridge: no queue access, tracking wait,
// or image submission. One shared-library instance for both native/Vulkan modules.
// Calls are serialized on Vulkan's caller thread: dispatching extension queries
// to a worker while inside vkCreateInstance can invert loader locks.
class RuntimeBootstrap {
    std::mutex mutex_;
    Session session_;
public:
    RuntimeBootstrap();
    ~RuntimeBootstrap();
    std::vector<std::string> instance_extensions();
    std::vector<std::string> device_extensions(VkPhysicalDevice_T* physical);
    VkPhysicalDevice_T* output_device(VkInstance_T* instance);
    FrameStatus sample_tracking(Matrix& tracking_from_head);
    // Stereo presentation (alternate-eye rendering). Seated pose predicted
    // `seconds` ahead; eye setup is constant per session.
    FrameStatus predicted_tracking(Matrix& tracking_from_head, float seconds);
    struct EyeSetup { std::array<Matrix, 2> head_from_eye; std::array<std::array<float, 4>, 2> tangents; }; // l,r,t,b
    EyeSetup eye_setup();
    // Submit both eye textures (with bounds), hand off, then block in WaitGetPoses
    // for pacing. Caller serializes the queue. Returns an empty string on success.
    // poses: seated head pose each eye image was rendered with (Submit_TextureWithPose);
    // with_pose false submits without poses (compositor assumes the WaitGetPoses pose).
    std::string submit_stereo(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                              const std::array<vr::VRTextureBounds_t, 2>& bounds,
                              const std::array<Matrix, 2>& poses, bool with_pose = true);
    // The same protocol split in two, for a dedicated submission thread.
    std::string wait_frame();
    std::string submit_frame(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                             const std::array<vr::VRTextureBounds_t, 2>& bounds,
                             const std::array<Matrix, 2>& poses, bool with_pose = true);
    // Theater mode: the flat game image on a world-fixed virtual screen (an OpenVR overlay),
    // `width` metres wide, centred at seated_from_screen. A null image keeps the last one.
    std::string show_theater(const vr::VRVulkanTextureData_t* image, const vr::VRTextureBounds_t& bounds,
                             const Matrix& seated_from_screen, float width);
    void hide_theater();
    // Mouse cursor overlay (X4 uses the Windows cursor, which never reaches the swapchain).
    // rgba: new image (null keeps the last). placement: cursor centre in seated space
    // (on_screen, on the theater screen) or relative to the headset; width in metres.
    std::string show_cursor(const uint8_t* rgba, uint32_t width, uint32_t height, bool on_screen,
                            const Matrix& placement, float width_m);
    void hide_cursor();
private:
    vr::VROverlayHandle_t theater_{vr::k_ulOverlayHandleInvalid}, cursor_{vr::k_ulOverlayHandleInvalid};
};
// Alternate-eye bookkeeping shared by the FreeTrack pose source and the Vulkan layer.
// Tunables are re-read from %X4VR_CAPTURE_DIR%/stereo.txt ("key=value" per line).
// delay = presents between the game reading a pose and presenting that frame.
struct StereoSettings { bool stereo = true; int delay = 2, recenter = 0; float ipd_scale = 1, pos_scale = 3.6f, yaw_gain = 2.1177f, pitch_gain = 2.1177f, roll_gain = 3.14159f, predict = 0.035f, game_tan_y = 0.8675f; // 0.8675 = X4 FOV slider at maximum (120 deg)
    // Calibration only: synthetic head pose (x y z metres, yaw pitch roll degrees, relative to
    // the recentred origin) plus a +/- delta alternating per game frame like the eyes.
    bool synth = false, pace = true, valve_bounds = true, pair = false;
    // Pair mode: hold the left eye's present until mid-way through the compositor frame.
    bool pair_wait = true;
    // Pose submitted with the eye images: 0 none, 1 each eye's own, 2 newest of the two for both
    // (2: inline submission only).
    int submit_pose = 1;
    // Submit from a dedicated thread every compositor frame (a late game frame repeats the
    // previous image instead of reaching SteamVR late), waiting at most submit_budget_ms
    // for the newest image's copy to finish. 0 = submit inline from the game's present.
    bool async_submit = true; float submit_budget_ms = 8;
    // Diagnostics: stall the game's render thread for hitch_ms once every hitch_every presents.
    float hitch_ms = 0; int hitch_every = 90;
    // Eye association by X4's per-frame render-data half instead of present counting.
    bool eye_from_half = false; int half_xor_render = 0, half_xor_present = 0; float synth_rate = 0; std::array<float, 6> synth_base{}, synth_alt{};
    // Theater mode (flat game image on a virtual screen): 0 off, 1 while X4 shows a fullscreen
    // menu or sends no head poses, 2 always. Screen distance and width in metres.
    int theater = 1; float theater_distance = 2.f, theater_width = 2.2f;
    // Mouse cursor overlay (1 on, 0 off); over the stereo view it sits cursor_distance metres ahead.
    int cursor = 1; float cursor_distance = 5.f; };
StereoSettings stereo_settings();
// Pose source (may be called several times per game frame): eye the frame being
// simulated now will be presented to, and the head pose it was rendered with.
uint32_t render_eye();
uint64_t frame_tag();   // present count when the game samples its pose
// flat: the frame shows a fullscreen menu (or theater mode is forced); it goes to the virtual screen.
void record_render_pose(const Matrix& head, uint32_t eye, bool flat = false);
// Recentred seated origin (position + yaw), published by the pose source; false until set.
void publish_view_origin(const Matrix& origin);
bool view_origin(Matrix& origin);
int frame_half(); // X4 9.00 per-frame double-buffer half (0/1), -1 if unavailable
// Layer: once per present. Returns the present number; pose lookup by number.
void trace_event(char kind, uint64_t value); // diagnostics: create trace.request to dump
uint64_t next_present();
// Eye, head pose and flat flag of the frame being presented now (false: no pose known).
bool presented_frame(uint64_t present, uint32_t& eye, Matrix& head, bool& flat);
// Avoid recursively bootstrapping if runtime initialization itself uses Vulkan.
bool is_runtime_bootstrap_thread();
std::shared_ptr<RuntimeBootstrap> acquire_runtime_bootstrap();
}
