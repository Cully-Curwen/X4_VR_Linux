#pragma once
// OpenXR backend behind RuntimeBootstrap (X4VR_RUNTIME=openxr). Takes the same OpenVR-shaped
// texture and bounds data the Vulkan layer builds for OpenVR, so the presenter stays shared.
#include <x4vr/runtime_bootstrap.hpp>
#include <openxr/openxr.h>
#include <memory>

namespace x4vr {
XrPosef to_xr(const Matrix& pose); // rotation normalised to a unit quaternion
Matrix from_xr(const XrPosef& pose);
// OpenVR tangents (left, right, top, bottom; top negative) <-> OpenXR angles (up positive).
XrFovf fov_from_tangents(const std::array<float, 4>& tangents);
std::array<float, 4> tangents_from_fov(const XrFovf& fov);
// Texture bounds (u right, v down, 0..1, geometric: not Valve's flipped v) -> pixel rectangle.
XrRect2Di bounds_rect(const vr::VRTextureBounds_t& bounds, uint32_t width, uint32_t height);
// Swapchain format for a copy of a `source` image: 8-bit UNORM becomes its SRGB twin (X4 stores
// display-ready gamma values; OpenXR reads UNORM as linear). 0 when the runtime offers neither.
int64_t swapchain_format(int64_t source, const std::vector<int64_t>& offered);

class OpenXRRuntime {
public:
    OpenXRRuntime(); // instance, system and Vulkan requirements; throws without a headset
    ~OpenXRRuntime();
    std::vector<std::string> instance_extensions();
    std::vector<std::string> device_extensions();
    VkPhysicalDevice_T* output_device(VkInstance_T* instance);
    // Session on the game's device and the layer's private queue; waits briefly for it to run.
    std::string start_session(const XrVulkanContext& vulkan);
    void end_session(VkDevice_T* device);
    FrameStatus predicted_tracking(Matrix& tracking_from_head, float seconds); // lock-free
    RuntimeBootstrap::EyeSetup eye_setup(); // empty until the session started
    std::string wait_frame();
    // with_pose false (theater's black eyes, submit_pose=0): each eye at the runtime's own pose for
    // this frame, like OpenVR's Submit_Default. Poses that aren't rigid are treated the same way.
    std::string submit_frame(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                             const std::array<vr::VRTextureBounds_t, 2>& bounds, const std::array<Matrix, 2>& poses,
                             bool with_pose);
    std::string show_theater(const vr::VRVulkanTextureData_t* image, const vr::VRTextureBounds_t& bounds,
                             const Matrix& seated_from_screen, float width);
    void hide_theater();
    std::string show_cursor(const uint8_t* rgba, uint32_t width, uint32_t height, bool on_screen,
                            const Matrix& placement, float width_m);
    void hide_cursor();
private:
    struct State;
    std::unique_ptr<State> s_;
};
}
