#include <x4vr/eye_targets.hpp>
#include <stdexcept>
#include <string>

namespace x4vr {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
template<class T> T resolve(const EyeTargetContext& c, const char* name) {
    auto function = reinterpret_cast<T>(c.resolve(c.device, name));
    if (!function) throw std::invalid_argument(std::string("Missing Vulkan function: ")+name);
    return function;
}
}
EyeTargets::EyeTargets(const EyeTargetContext& context, VkExtent2D extent, VkFormat color, VkFormat depth)
    : context_(context), extent_(extent), color_format_(color), depth_format_(depth) {
    if (!context.device || !context.physical || !context.resolve || !context.memory_properties || !context.image_properties)
        throw std::invalid_argument("Incomplete eye-target context");
    if (!extent.width || !extent.height) throw std::invalid_argument("Empty eye-target extent");
    if (color != VK_FORMAT_R8G8B8A8_UNORM && color != VK_FORMAT_R8G8B8A8_SRGB &&
        color != VK_FORMAT_B8G8R8A8_UNORM && color != VK_FORMAT_B8G8R8A8_SRGB &&
        color != VK_FORMAT_R16G16B16A16_SFLOAT)
        throw std::invalid_argument("Unsupported eye color format");
    // Stencil variants require a different aspect/view contract. Add explicitly
    // when X4's target layout is known, rather than silently dropping stencil.
    if (depth != VK_FORMAT_D32_SFLOAT && depth != VK_FORMAT_D16_UNORM)
        throw std::invalid_argument("Unsupported eye depth format");
    create_image_ = resolve<PFN_vkCreateImage>(context, "vkCreateImage");
    destroy_image_ = resolve<PFN_vkDestroyImage>(context, "vkDestroyImage");
    requirements_ = resolve<PFN_vkGetImageMemoryRequirements2>(context, "vkGetImageMemoryRequirements2");
    allocate_ = resolve<PFN_vkAllocateMemory>(context, "vkAllocateMemory");
    free_ = resolve<PFN_vkFreeMemory>(context, "vkFreeMemory");
    bind_ = resolve<PFN_vkBindImageMemory>(context, "vkBindImageMemory");
    create_view_ = resolve<PFN_vkCreateImageView>(context, "vkCreateImageView");
    destroy_view_ = resolve<PFN_vkDestroyImageView>(context, "vkDestroyImageView");
}
std::unique_ptr<EyeTargets> EyeTargets::create(const EyeTargetContext& context,
    VkExtent2D extent, VkFormat color, VkFormat depth) {
    auto result = std::unique_ptr<EyeTargets>(new EyeTargets(context, extent, color, depth));
    result->initialize(); // unique_ptr rolls back even if the fourth attachment fails
    return result;
}
void EyeTargets::initialize() {
    context_.memory_properties(context_.physical, &memory_);
    if (!memory_.memoryTypeCount || memory_.memoryTypeCount > VK_MAX_MEMORY_TYPES)
        throw std::runtime_error("Invalid Vulkan memory types");
    for (auto& eye : eyes_) {
        create_attachment(eye.color, color_format_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        create_attachment(eye.depth, depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}
void EyeTargets::create_attachment(EyeAttachment& target, VkFormat format,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageFormatProperties supported{};
    check(context_.image_properties(context_.physical, format, VK_IMAGE_TYPE_2D,
          VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "eye image format query");
    if (extent_.width > supported.maxExtent.width || extent_.height > supported.maxExtent.height ||
        !(supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
        throw std::invalid_argument("Eye extent/sample count exceeds format limits");
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D; image.format = format;
    image.extent = {extent_.width, extent_.height, 1};
    image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = usage;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE; image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage created_image{};
    check(create_image_(context_.device, &image, nullptr, &created_image), "create eye attachment");
    target.image = created_image;
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; requirements.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2}; info.image = target.image;
    requirements_(context_.device, &info, &requirements);
    uint32_t type = 0;
    while (type < memory_.memoryTypeCount &&
           (!(requirements.memoryRequirements.memoryTypeBits & (1u << type)) ||
            !(memory_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++type;
    if (type == memory_.memoryTypeCount) throw std::runtime_error("No device-local eye attachment memory");
    // Each image always receives its own dedicated allocation, satisfying both
    // required and preferred dedication without aliasing the two eye targets.
    VkMemoryDedicatedAllocateInfo allocation_image{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    allocation_image.image = target.image;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &allocation_image;
    allocation.allocationSize = requirements.memoryRequirements.size; allocation.memoryTypeIndex = type;
    VkDeviceMemory created_memory{};
    check(allocate_(context_.device, &allocation, nullptr, &created_memory), "allocate eye attachment memory");
    target.memory = created_memory;
    check(bind_(context_.device, target.image, target.memory, 0), "bind eye attachment memory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView created_view{};
    check(create_view_(context_.device, &view, nullptr, &created_view), "create eye attachment view");
    target.view = created_view;
}
void EyeTargets::destroy_attachment(EyeAttachment& target) noexcept {
    if (target.view) destroy_view_(context_.device, target.view, nullptr);
    if (target.image) destroy_image_(context_.device, target.image, nullptr);
    if (target.memory) free_(context_.device, target.memory, nullptr);
    target = {};
}
EyeTargets::~EyeTargets() {
    for (auto& eye : eyes_) { destroy_attachment(eye.depth); destroy_attachment(eye.color); }
}
}
