#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <memory>

namespace x4vr {
struct EyeTargetContext {
    VkDevice device{};
    VkPhysicalDevice physical{};
    PFN_vkGetDeviceProcAddr resolve{};
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties{};
    PFN_vkGetPhysicalDeviceImageFormatProperties image_properties{};
};
struct EyeAttachment {
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
};
struct EyeTarget {
    EyeAttachment color;
    EyeAttachment depth;
};
// Owns attachments, NOT the host instance/device/queue. Vulkan 1.1 is required.
// The caller releases OpenVR references and completes GPU work before destruction.
// No implicit device-wide idle wait, queue submission, or image layout tracking.
// Construction is transactional: all partial allocations unwind on failure.
class EyeTargets {
public:
    static std::unique_ptr<EyeTargets> create(const EyeTargetContext& context,
        VkExtent2D extent, VkFormat color_format, VkFormat depth_format = VK_FORMAT_D32_SFLOAT);
    ~EyeTargets();
    EyeTargets(const EyeTargets&) = delete;
    EyeTargets& operator=(const EyeTargets&) = delete;
    const std::array<EyeTarget, 2>& eyes() const { return eyes_; }
    VkExtent2D extent() const { return extent_; }
    VkFormat color_format() const { return color_format_; }
    VkFormat depth_format() const { return depth_format_; }
private:
    EyeTargets(const EyeTargetContext&, VkExtent2D, VkFormat, VkFormat);
    void initialize();
    void create_attachment(EyeAttachment&, VkFormat, VkImageUsageFlags, VkImageAspectFlags);
    void destroy_attachment(EyeAttachment&) noexcept;
    EyeTargetContext context_;
    VkExtent2D extent_;
    VkFormat color_format_, depth_format_;
    std::array<EyeTarget, 2> eyes_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    PFN_vkCreateImage create_image_{};
    PFN_vkDestroyImage destroy_image_{};
    PFN_vkGetImageMemoryRequirements2 requirements_{};
    PFN_vkAllocateMemory allocate_{};
    PFN_vkFreeMemory free_{};
    PFN_vkBindImageMemory bind_{};
    PFN_vkCreateImageView create_view_{};
    PFN_vkDestroyImageView destroy_view_{};
};
}
