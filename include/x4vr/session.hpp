#pragma once
#include <x4vr/math.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace x4vr {
struct EyeView {
    Matrix seated_from_eye;
    Matrix view;
    Matrix projection;
};
struct Frame {
    uint64_t id = 0;
    uint32_t width = 0, height = 0;
    std::array<EyeView, 2> eyes;
};
enum class FrameStatus { ready, tracking_unavailable, compositor_unavailable, quit_requested };

// Validates metadata only. Cannot establish that a VkImage is alive, in the
// correct layout, synchronized, or contains a geometrically correct eye view.
std::string validate_images(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                            uint32_t width, uint32_t height);

// One session per process. All methods, including destruction, run on its owner
// (render) thread. Caller must serialize ALL access to the supplied VkQueue,
// including WaitGetPoses, Submit and PostPresentHandoff. Shutdown before destroying
// any Vulkan resources handed to OpenVR. Exceptions stay within the C++ host.
class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void initialize();
    void shutdown();
    std::vector<std::string> instance_extensions() const;
    std::vector<std::string> device_extensions(VkPhysicalDevice_T* physical) const;
    VkPhysicalDevice_T* output_device(VkInstance_T* instance) const;
    std::string headset_model() const;
    void request_recenter();
    // Current raw seated pose, without compositor pacing, submission or queue
    // access. This diagnostic path must not replace WaitGetPoses for VR rendering.
    FrameStatus sample_tracking(Matrix& tracking_from_head);
    FrameStatus begin_frame(Frame& frame, float near_m, float far_m, bool reverse_z = false);
    void submit(uint64_t frame_id, const std::array<vr::VRVulkanTextureData_t, 2>& images,
                vr::EColorSpace color_space = vr::ColorSpace_Auto);
    void post_present();
    const std::string& last_error() const { return error_; }
private:
    friend class RuntimeBootstrap;
    // No frame/queue operations exist during bootstrap. Its mutex permits
    // initialization ownership to follow Vulkan's caller, avoiding loader locks.
    void adopt_bootstrap_thread();
    void check_owner() const;
    bool poll_quit();
    vr::IVRSystem* system_ = nullptr;
    vr::IVRCompositor* compositor_ = nullptr;
    std::thread::id owner_;
    uint64_t sequence_ = 0, pending_ = 0;
    uint32_t width_ = 0, height_ = 0;
    bool recenter_ = true, quit_ = false, handoff_ = false;
    Matrix seated_from_tracking_ = Matrix::identity();
    std::string error_;
};
}
