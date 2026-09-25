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
    // poses: seated head pose each eye image was rendered with (Submit_TextureWithPose).
    std::string submit_stereo(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                              const std::array<vr::VRTextureBounds_t, 2>& bounds,
                              const std::array<Matrix, 2>& poses);
};
// Alternate-eye bookkeeping shared by the FreeTrack pose source and the Vulkan layer.
// Tunables are re-read from %X4VR_CAPTURE_DIR%/stereo.txt ("key=value" per line).
// delay = presents between the game reading a pose and presenting that frame.
struct StereoSettings { bool stereo = true; int delay = 2, recenter = 0; float ipd_scale = 1, pos_scale = 3.6f, yaw_gain = 2.1177f, pitch_gain = 2.1177f, roll_gain = 3.14159f, predict = 0.035f, game_tan_y = 0.8675f; // 0.8675 = X4 FOV slider at maximum (120 deg)
    // Calibration only: synthetic head pose (x y z metres, yaw pitch roll degrees, relative to
    // the recentred origin) plus a +/- delta alternating per game frame like the eyes.
    bool synth = false, pace = true, valve_bounds = true; float synth_rate = 0; std::array<float, 6> synth_base{}, synth_alt{}; };
StereoSettings stereo_settings();
// Pose source (may be called several times per game frame): eye the frame being
// simulated now will be presented to, and the head pose it was rendered with.
uint32_t render_eye();
uint64_t frame_tag();   // present count when the game samples its pose
void record_render_pose(const Matrix& head);
// Layer: once per present. Returns the present number; pose lookup by number.
uint64_t next_present();
bool render_pose_for(uint64_t present, Matrix& head);
// Avoid recursively bootstrapping if runtime initialization itself uses Vulkan.
bool is_runtime_bootstrap_thread();
std::shared_ptr<RuntimeBootstrap> acquire_runtime_bootstrap();
}
