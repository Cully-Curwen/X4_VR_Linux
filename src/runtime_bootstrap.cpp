#include <x4vr/runtime_bootstrap.hpp>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace x4vr {
namespace {
thread_local bool runtime_thread = false;
struct RuntimeCall {
    bool previous = runtime_thread;
    RuntimeCall() { runtime_thread = true; }
    ~RuntimeCall() { runtime_thread = previous; }
};
std::mutex bootstrap_mutex;
std::weak_ptr<RuntimeBootstrap> bootstrap;
}
bool is_runtime_bootstrap_thread() { return runtime_thread; }
RuntimeBootstrap::RuntimeBootstrap() {
    RuntimeCall call;
    OutputDebugStringA("X4VR bootstrap: initialize begin\n");
    session_.initialize();
    OutputDebugStringA("X4VR bootstrap: initialize complete\n");
}
RuntimeBootstrap::~RuntimeBootstrap() {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    session_.shutdown();
}
std::vector<std::string> RuntimeBootstrap::instance_extensions() {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    OutputDebugStringA("X4VR bootstrap: instance extensions begin\n");
    auto result = session_.instance_extensions();
    OutputDebugStringA("X4VR bootstrap: instance extensions complete\n");
    return result;
}
std::vector<std::string> RuntimeBootstrap::device_extensions(VkPhysicalDevice_T* physical) {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    OutputDebugStringA("X4VR bootstrap: device extensions begin\n");
    auto result = session_.device_extensions(physical);
    OutputDebugStringA("X4VR bootstrap: device extensions complete\n");
    return result;
}
VkPhysicalDevice_T* RuntimeBootstrap::output_device(VkInstance_T* instance) {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    OutputDebugStringA("X4VR bootstrap: output GPU begin\n");
    auto result = session_.output_device(instance);
    OutputDebugStringA("X4VR bootstrap: output GPU complete\n");
    return result;
}
std::shared_ptr<RuntimeBootstrap> acquire_runtime_bootstrap() {
    std::lock_guard lock(bootstrap_mutex);
    auto result = bootstrap.lock();
    if (!result) { result = std::make_shared<RuntimeBootstrap>(); bootstrap = result; }
    // Pin for the process lifetime: X4 creates and destroys temporary Vulkan
    // instances at startup, and VR_Init/VR_Shutdown churn (vrclient DLL load/unload
    // plus its threads) raced a loader-lock deadlock with overlay hooks. Deliberately
    // leaked: no VR_Shutdown from static destruction; the OS reclaims at exit.
    static auto* pinned = new std::shared_ptr<RuntimeBootstrap>(result);
    (void)pinned;
    return result;
}
FrameStatus RuntimeBootstrap::sample_tracking(Matrix& tracking_from_head) {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    return session_.sample_tracking(tracking_from_head);
}
FrameStatus RuntimeBootstrap::predicted_tracking(Matrix& tracking_from_head, float seconds) {
    // Deliberately lock-free: the game thread must never wait behind the render
    // thread's WaitGetPoses. IVRSystem pose queries are safe from any thread.
    tracking_from_head = {};
    vr::TrackedDevicePose_t head{};
    session_.system_->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, seconds, &head, 1);
    const auto pose = from_pose(head.mDeviceToAbsoluteTracking);
    if (!head.bDeviceIsConnected || !head.bPoseIsValid || !is_rigid(pose)) return FrameStatus::tracking_unavailable;
    tracking_from_head = pose;
    return FrameStatus::ready;
}
RuntimeBootstrap::EyeSetup RuntimeBootstrap::eye_setup() {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    EyeSetup setup{};
    for (int i = 0; i < 2; ++i) {
        const auto eye = static_cast<vr::EVREye>(i);
        setup.head_from_eye[i] = from_pose(session_.system_->GetEyeToHeadTransform(eye));
        auto& t = setup.tangents[i];
        session_.system_->GetProjectionRaw(eye, &t[0], &t[1], &t[2], &t[3]);
    }
    return setup;
}
std::string RuntimeBootstrap::submit_stereo(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                                            const std::array<vr::VRTextureBounds_t, 2>& bounds,
                                            const std::array<Matrix, 2>& poses) {
    std::lock_guard lock(mutex_);
    RuntimeCall call;
    session_.adopt_bootstrap_thread();
    if (session_.poll_quit()) return "OpenVR quit requested";
    // Frame protocol: WaitGetPoses -> Submit(both) -> PostPresentHandoff. Waiting
    // first also paces the game to the headset refresh.
    std::array<vr::TrackedDevicePose_t, 1> tracked{};
    auto error = session_.compositor_->WaitGetPoses(tracked.data(), 1, nullptr, 0);
    if (error != vr::VRCompositorError_None) return "WaitGetPoses failed: OpenVR compositor error " + std::to_string(error);
    auto descriptors = images;
    for (int i = 0; i < 2; ++i) {
        vr::VRTextureWithPose_t texture{};
        texture.handle = &descriptors[i]; texture.eType = vr::TextureType_Vulkan; texture.eColorSpace = vr::ColorSpace_Auto;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) texture.mDeviceToAbsoluteTracking.m[r][c] = poses[i].m[r][c];
        error = session_.compositor_->Submit(static_cast<vr::EVREye>(i), &texture, &bounds[i], vr::Submit_TextureWithPose);
        if (error != vr::VRCompositorError_None) return "Submit failed: OpenVR compositor error " + std::to_string(error);
    }
    session_.compositor_->PostPresentHandoff();
    return {};
}
namespace {
std::atomic_uint64_t present_frames{};
struct TaggedPose { uint64_t tag = ~0ull; Matrix head; };
std::mutex poses_mutex;
std::array<TaggedPose, 16> poses;
std::mutex settings_mutex;
StereoSettings cached;
std::chrono::steady_clock::time_point checked{};
}
StereoSettings stereo_settings() {
    std::lock_guard lock(settings_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now-checked < std::chrono::milliseconds(500)) return cached;
    checked = now;
    char root[1024]{};
    if (!GetEnvironmentVariableA("X4VR_CAPTURE_DIR", root, sizeof(root))) return cached;
    std::ifstream file(std::string(root)+"/stereo.txt");
    StereoSettings next;
    for (std::string line; std::getline(file, line);) {
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const auto key = line.substr(0, split);
        const auto value = std::atof(line.c_str()+split+1);
        if (key == "stereo") next.stereo = value != 0;
        else if (key == "delay") next.delay = static_cast<int>(value);
        else if (key == "ipd_scale") next.ipd_scale = static_cast<float>(value);
        else if (key == "yaw_gain") next.yaw_gain = static_cast<float>(value);
        else if (key == "pitch_gain") next.pitch_gain = static_cast<float>(value);
        else if (key == "roll_gain") next.roll_gain = static_cast<float>(value);
        else if (key == "recenter") next.recenter = static_cast<int>(value);
        else if (key == "pos_scale") next.pos_scale = static_cast<float>(value);
        else if (key == "predict") next.predict = static_cast<float>(value);
        else if (key == "game_tan_y") next.game_tan_y = static_cast<float>(value);
        else if (key == "synth") next.synth = value != 0;
        else if (key == "pace") next.pace = value != 0;
        else if (key == "valve_bounds") next.valve_bounds = value != 0;
        else if (key == "synth_rate") next.synth_rate = static_cast<float>(value);
        else if (key == "synth_base" || key == "synth_alt") {
            auto& target = key == "synth_base" ? next.synth_base : next.synth_alt;
            std::istringstream in(line.substr(split+1));
            for (auto& v : target) in >> v;
        }
    }
    cached = next;
    return cached;
}
uint32_t render_eye() { return static_cast<uint32_t>((present_frames.load()+stereo_settings().delay) & 1); }
void record_render_pose(const Matrix& head) {
    const auto tag = present_frames.load();
    std::lock_guard lock(poses_mutex);
    poses[tag % poses.size()] = {tag, head}; // several calls per frame: last one wins
}
uint64_t next_present() { return present_frames++; }
uint64_t frame_tag() { return present_frames.load(); }
bool render_pose_for(uint64_t present, Matrix& head) {
    const auto tag = present - static_cast<uint64_t>(stereo_settings().delay);
    std::lock_guard lock(poses_mutex);
    const auto& entry = poses[tag % poses.size()];
    if (entry.tag != tag) return false;
    head = entry.head;
    return true;
}
}
