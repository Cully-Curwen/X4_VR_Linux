#include <x4vr/eye_targets.hpp>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
int step{}, fail_at{};
uintptr_t serial = 16;
bool contracts = true, no_memory = false;
std::map<VkImage, VkDeviceMemory> images;
std::map<VkDeviceMemory, VkImage> allocations;
std::map<VkImageView, VkImage> views;
template<class T> T fake() { return reinterpret_cast<T>(++serial); }
void verify(bool value) { contracts = contracts && value; }
bool fail() { return ++step == fail_at; }
VKAPI_ATTR VkResult VKAPI_CALL image_properties(VkPhysicalDevice, VkFormat, VkImageType,
    VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties* out) {
    if (fail()) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out = {}; out->maxExtent = {4096,4096,1}; out->maxArrayLayers = out->maxMipLevels = 1;
    out->sampleCounts = VK_SAMPLE_COUNT_1_BIT; out->maxResourceSize = 1ull<<32;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL memory_properties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    *out = {}; out->memoryTypeCount = 2;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    out->memoryTypes[1].propertyFlags = no_memory ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice, const VkImageCreateInfo* ci,
    const VkAllocationCallbacks*, VkImage* out) {
    if (fail()) { *out = reinterpret_cast<VkImage>(0xdead); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    verify(ci->samples == VK_SAMPLE_COUNT_1_BIT && ci->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    const VkImageUsageFlags required = ci->format == VK_FORMAT_D32_SFLOAT ?
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    verify((ci->usage & required) == required && (ci->usage & VK_IMAGE_USAGE_SAMPLED_BIT));
    *out = fake<VkImage>(); images[*out] = {}; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice, VkImage image, const VkAllocationCallbacks*) {
    verify(images.contains(image));
    for (const auto& [view, owner] : views) { (void)view; verify(owner != image); }
    images.erase(image);
}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* out) {
    verify(images.contains(info->image));
    auto* dedicated = static_cast<VkMemoryDedicatedRequirements*>(out->pNext);
    verify(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    dedicated->requiresDedicatedAllocation = VK_TRUE;
    out->memoryRequirements = {4096, 256, 2};
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks*, VkDeviceMemory* out) {
    if (fail()) { *out = reinterpret_cast<VkDeviceMemory>(0xdead); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    const auto* dedicated = static_cast<const VkMemoryDedicatedAllocateInfo*>(info->pNext);
    verify(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO &&
           images.contains(dedicated->image) && !dedicated->buffer);
    verify(info->memoryTypeIndex == 1 && info->allocationSize == 4096);
    *out = fake<VkDeviceMemory>(); allocations[*out] = dedicated->image; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    verify(allocations.contains(memory) && !images.contains(allocations[memory]));
    allocations.erase(memory);
}
VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    if (fail()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    verify(images.contains(image) && allocations[memory] == image && offset == 0);
    images[image] = memory; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice, const VkImageViewCreateInfo* ci,
    const VkAllocationCallbacks*, VkImageView* out) {
    if (fail()) { *out = reinterpret_cast<VkImageView>(0xdead); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    verify(images.contains(ci->image) && images[ci->image]);
    const VkImageAspectFlags aspect = ci->format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    verify(ci->subresourceRange.aspectMask == aspect);
    *out = fake<VkImageView>(); views[*out] = ci->image; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice, VkImageView view, const VkAllocationCallbacks*) {
    verify(views.contains(view)); views.erase(view);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolve(VkDevice, const char* name) {
#define FN(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<PFN_vkVoidFunction>(f)
    FN("vkCreateImage", create_image); FN("vkDestroyImage", destroy_image);
    FN("vkGetImageMemoryRequirements2", requirements); FN("vkAllocateMemory", allocate);
    FN("vkFreeMemory", free_memory); FN("vkBindImageMemory", bind);
    FN("vkCreateImageView", create_view); FN("vkDestroyImageView", destroy_view);
#undef FN
    return nullptr;
}
void require(bool value) { if (!value) throw std::runtime_error("Eye target contract failed"); }
bool empty() { return images.empty() && views.empty() && allocations.empty(); }
}
int main() {
    try {
        const x4vr::EyeTargetContext context{fake<VkDevice>(), fake<VkPhysicalDevice>(), resolve, memory_properties, image_properties};
        {
            auto targets = x4vr::EyeTargets::create(context, {3292,2820}, VK_FORMAT_R8G8B8A8_UNORM);
            require(images.size() == 4 && allocations.size() == 4 && views.size() == 4);
            require(targets->eyes()[0].color.image != targets->eyes()[1].color.image);
            require(targets->eyes()[0].depth.image != targets->eyes()[1].depth.image);
            require(targets->extent().width == 3292);
        }
        const int operations = step;
        require(operations == 20 && contracts && empty());
        for (int failure = 1; failure <= operations; ++failure) {
            step = 0; fail_at = failure;
            bool rejected = false;
            try { auto targets = x4vr::EyeTargets::create(context, {3292,2820}, VK_FORMAT_R8G8B8A8_UNORM); }
            catch (const std::runtime_error&) { rejected = true; }
            require(rejected && empty() && contracts);
        }
        fail_at = 0; no_memory = true;
        bool rejected = false;
        try { auto targets = x4vr::EyeTargets::create(context, {32,32}, VK_FORMAT_R8G8B8A8_UNORM); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected && empty() && contracts);
        no_memory = false; rejected = false;
        try { auto targets = x4vr::EyeTargets::create(context, {0,32}, VK_FORMAT_R8G8B8A8_UNORM); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && empty());
        std::cout << "Paired color/depth ownership and all 20 injected failures passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
