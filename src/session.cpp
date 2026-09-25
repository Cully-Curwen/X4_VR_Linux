#include <x4vr/session.hpp>
#include <atomic>
#include <sstream>
#include <stdexcept>

namespace x4vr {
namespace {
std::atomic_flag active_session = ATOMIC_FLAG_INIT;
template<class Query> std::vector<std::string> extensions(Query query) {
    const uint32_t count = query(nullptr, 0);
    if (count == 0 || count > 1024*1024) throw std::runtime_error("OpenVR extension query failed");
    std::vector<char> text(count, '\0');
    const auto actual = query(text.data(), count);
    if (actual == 0 || actual > count || text.back() != '\0')
        throw std::runtime_error("OpenVR extension query changed or was truncated");
    std::istringstream input(text.data());
    std::vector<std::string> result;
    for (std::string extension; input >> extension;) result.push_back(extension);
    return result;
}
std::string compositor_error(const char* operation, vr::EVRCompositorError error) {
    return std::string(operation) + " failed: OpenVR compositor error " + std::to_string(error);
}
}
std::string validate_images(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                            uint32_t width, uint32_t height) {
    if (!width || !height) return "No render-target extent";
    for (const auto& i : images) {
        if (!i.m_nImage || !i.m_pDevice || !i.m_pPhysicalDevice || !i.m_pInstance || !i.m_pQueue)
            return "Missing Vulkan image or context";
        if (i.m_nWidth != width || i.m_nHeight != height) return "Eye image size differs from frame extent";
        if (i.m_nSampleCount != 1) return "Resolve multisampled eye images before submission";
        if (!i.m_nFormat) return "Undefined eye image format";
    }
    const auto& l = images[0]; const auto& r = images[1];
    if (l.m_pDevice != r.m_pDevice || l.m_pPhysicalDevice != r.m_pPhysicalDevice ||
        l.m_pInstance != r.m_pInstance || l.m_pQueue != r.m_pQueue ||
        l.m_nQueueFamilyIndex != r.m_nQueueFamilyIndex || l.m_nFormat != r.m_nFormat)
        return "Eye images must share their Vulkan context, queue and format";
    if (l.m_nImage == r.m_nImage) return "Separate eye images required by this backend";
    return {};
}
Session::~Session() {
    // Contract violations cannot safely be recovered during renderer teardown.
    if (system_ && owner_ != std::this_thread::get_id()) std::terminate();
    shutdown();
}
void Session::check_owner() const {
    if (!system_) throw std::logic_error("OpenVR session is not initialized");
    if (owner_ != std::this_thread::get_id()) throw std::logic_error("Wrong OpenVR render thread");
}
void Session::adopt_bootstrap_thread() {
    if (sequence_ || pending_ || handoff_) throw std::logic_error("Cannot migrate an active renderer session");
    owner_ = std::this_thread::get_id();
}
void Session::initialize() {
    if (system_) throw std::logic_error("OpenVR session already initialized");
    if (active_session.test_and_set()) throw std::logic_error("Another OpenVR session is active");
    vr::EVRInitError err = vr::VRInitError_None;
    system_ = vr::VR_Init(&err, vr::VRApplication_Scene);
    if (err != vr::VRInitError_None || !system_) {
        system_ = nullptr;
        active_session.clear();
        throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(err));
    }
    owner_ = std::this_thread::get_id();
    compositor_ = vr::VRCompositor();
    if (!compositor_) {
        shutdown();
        throw std::runtime_error("OpenVR compositor interface is unavailable");
    }
    compositor_->SetTrackingSpace(vr::TrackingUniverseSeated);
    recenter_ = true; quit_ = false; handoff_ = false;
    seated_from_tracking_ = Matrix::identity();
    error_.clear();
}
void Session::shutdown() {
    if (!system_) return;
    check_owner();
    if (compositor_) compositor_->ClearLastSubmittedFrame();
    vr::VR_Shutdown();
    system_ = nullptr; compositor_ = nullptr;
    pending_ = 0; handoff_ = false;
    active_session.clear();
}
std::vector<std::string> Session::instance_extensions() const {
    check_owner();
    return extensions([&](char* text, uint32_t size) {
        return compositor_->GetVulkanInstanceExtensionsRequired(text, size);
    });
}
std::vector<std::string> Session::device_extensions(VkPhysicalDevice_T* physical) const {
    check_owner();
    if (!physical) throw std::invalid_argument("Null Vulkan physical device");
    return extensions([&](char* text, uint32_t size) {
        return compositor_->GetVulkanDeviceExtensionsRequired(physical, text, size);
    });
}
VkPhysicalDevice_T* Session::output_device(VkInstance_T* instance) const {
    check_owner();
    if (!instance) throw std::invalid_argument("Null Vulkan instance");
    uint64_t device = 0;
    system_->GetOutputDevice(&device, vr::TextureType_Vulkan, instance);
    if (!device) throw std::runtime_error("OpenVR did not identify the headset GPU");
    return reinterpret_cast<VkPhysicalDevice_T*>(device);
}
std::string Session::headset_model() const {
    check_owner();
    std::array<char, vr::k_unMaxPropertyStringSize> text{};
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    system_->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_ModelNumber_String, text.data(), static_cast<uint32_t>(text.size()), &error);
    if (error != vr::TrackedProp_Success) throw std::runtime_error("Headset model query failed");
    return text.data();
}
void Session::request_recenter() { check_owner(); recenter_ = true; }
bool Session::poll_quit() {
    vr::VREvent_t event{};
    while (system_->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_Quit) {
            system_->AcknowledgeQuit_Exiting(); quit_ = true;
        }
    }
    return quit_;
}
FrameStatus Session::sample_tracking(Matrix& tracking_from_head) {
    check_owner();
    tracking_from_head = {};
    if (poll_quit()) return FrameStatus::quit_requested;
    vr::TrackedDevicePose_t head{};
    system_->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0.f, &head, 1);
    const auto pose = from_pose(head.mDeviceToAbsoluteTracking);
    if (!head.bDeviceIsConnected || !head.bPoseIsValid || !is_rigid(pose))
        return FrameStatus::tracking_unavailable;
    tracking_from_head = pose;
    return FrameStatus::ready;
}
FrameStatus Session::begin_frame(Frame& frame, float near_m, float far_m, bool reverse_z) {
    check_owner();
    frame = {}; pending_ = 0; handoff_ = false; error_.clear();
    // Validate caller parameters before blocking on the compositor.
    (void)vulkan_projection(-1, 1, -1, 1, near_m, far_m, reverse_z);
    if (poll_quit()) return FrameStatus::quit_requested;
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
    const auto err = compositor_->WaitGetPoses(poses.data(), static_cast<uint32_t>(poses.size()), nullptr, 0);
    if (err != vr::VRCompositorError_None) {
        error_ = compositor_error("WaitGetPoses", err);
        return FrameStatus::compositor_unavailable;
    }
    const auto& head = poses[vr::k_unTrackedDeviceIndex_Hmd];
    const auto tracking_from_head = from_pose(head.mDeviceToAbsoluteTracking);
    if (!head.bDeviceIsConnected || !head.bPoseIsValid || !is_rigid(tracking_from_head)) {
        compositor_->ClearLastSubmittedFrame();
        error_ = "Headset tracking unavailable: connected=" + std::to_string(head.bDeviceIsConnected)
               + ", valid_pose=" + std::to_string(head.bPoseIsValid)
               + ", tracking_result=" + std::to_string(head.eTrackingResult);
        return FrameStatus::tracking_unavailable;
    }
    if (recenter_) {
        seated_from_tracking_ = inverse_rigid(seated_origin(tracking_from_head));
        recenter_ = false;
    }
    system_->GetRecommendedRenderTargetSize(&width_, &height_);
    if (!width_ || !height_) throw std::runtime_error("OpenVR returned an empty render extent");
    Frame next;
    next.width = width_; next.height = height_;
    for (int i = 0; i < 2; ++i) {
        const auto eye = static_cast<vr::EVREye>(i);
        const auto head_from_eye = from_pose(system_->GetEyeToHeadTransform(eye));
        auto& view = next.eyes[i];
        view.seated_from_eye = multiply(multiply(seated_from_tracking_, tracking_from_head), head_from_eye);
        view.view = inverse_rigid(view.seated_from_eye);
        float left, right, top, bottom;
        system_->GetProjectionRaw(eye, &left, &right, &top, &bottom);
        view.projection = vulkan_projection(left, right, top, bottom, near_m, far_m, reverse_z);
    }
    next.id = ++sequence_; pending_ = next.id;
    frame = next;
    return FrameStatus::ready;
}
void Session::submit(uint64_t id, const std::array<vr::VRVulkanTextureData_t, 2>& images,
                     vr::EColorSpace color_space) {
    check_owner();
    if (!id || id != pending_) throw std::logic_error("Stale, invalid or already submitted frame");
    if (const auto problem = validate_images(images, width_, height_); !problem.empty())
        throw std::invalid_argument(problem);
    if (color_space != vr::ColorSpace_Auto && color_space != vr::ColorSpace_Gamma &&
        color_space != vr::ColorSpace_Linear) throw std::invalid_argument("Invalid color space");
    if (images[0].m_pPhysicalDevice != output_device(images[0].m_pInstance))
        throw std::invalid_argument("Eye images are not on the headset GPU");
    // Consume even if one eye fails: retrying an already accepted eye is invalid.
    pending_ = 0;
    auto descriptors = images;
    for (int i = 0; i < 2; ++i) {
        const vr::Texture_t texture{&descriptors[i], vr::TextureType_Vulkan, color_space};
        const auto err = compositor_->Submit(static_cast<vr::EVREye>(i), &texture);
        if (err != vr::VRCompositorError_None) {
            compositor_->ClearLastSubmittedFrame();
            error_ = compositor_error(i == 0 ? "Submit left" : "Submit right", err);
            throw std::runtime_error(error_);
        }
    }
    handoff_ = true;
}
void Session::post_present() {
    check_owner();
    if (!handoff_) throw std::logic_error("No submitted stereo frame awaiting presentation");
    compositor_->PostPresentHandoff();
    handoff_ = false;
}
}
