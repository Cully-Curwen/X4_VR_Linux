// Submission/lifetime diagnostic, NOT a stereo scene or X4 integration.
#include <windows.h>
#include <vulkan/vulkan.h>
#include <x4vr/session.hpp>
#include <x4vr/eye_targets.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
template<class F, class Resolver, class Object> F resolve(Resolver resolver, Object object, const char* name) {
    auto f = reinterpret_cast<F>(resolver(object, name));
    if (!f) throw std::runtime_error(std::string("Missing Vulkan function ")+name);
    return f;
}
std::vector<const char*> names(const std::vector<std::string>& strings) {
    std::vector<const char*> result;
    for (const auto& s : strings) result.push_back(s.c_str());
    return result;
}
struct Probe {
    x4vr::Session session;
    HMODULE library{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{}, width{}, height{};
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkCommandPool pool{};
    VkCommandBuffer command{};
    std::unique_ptr<x4vr::EyeTargets> targets;
    VkRenderPass render_pass{};
    std::array<VkFramebuffer, 2> framebuffers{};
    PFN_vkDestroyInstance DestroyInstance{};
#define DEVICE_FUNCTIONS(F) F(DestroyDevice) F(DeviceWaitIdle) \
    F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(AllocateMemory) F(FreeMemory) \
    F(BindBufferMemory) F(MapMemory) F(UnmapMemory) F(CmdCopyImageToBuffer) \
    F(CreateRenderPass) F(DestroyRenderPass) F(CreateFramebuffer) F(DestroyFramebuffer) \
    F(CmdBeginRenderPass) F(CmdEndRenderPass) \
    F(CreateCommandPool) F(DestroyCommandPool) F(AllocateCommandBuffers) F(ResetCommandPool) \
    F(BeginCommandBuffer) F(EndCommandBuffer) F(CmdPipelineBarrier) F(CmdClearColorImage) \
    F(GetDeviceQueue) F(QueueSubmit) F(QueueWaitIdle)
#define MEMBER(name) PFN_vk##name name{};
    DEVICE_FUNCTIONS(MEMBER)
#undef MEMBER
    ~Probe() {
        // Runtime releases eye images before any associated Vulkan object dies.
        session.shutdown();
        if (device && DeviceWaitIdle) DeviceWaitIdle(device);
        if (pool && DestroyCommandPool) DestroyCommandPool(device, pool, nullptr);
        for (const auto framebuffer : framebuffers)
            if (framebuffer && DestroyFramebuffer) DestroyFramebuffer(device, framebuffer, nullptr);
        if (render_pass && DestroyRenderPass) DestroyRenderPass(device, render_pass, nullptr);
        targets.reset();
        if (device && DestroyDevice) DestroyDevice(device, nullptr);
        if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
        if (library) FreeLibrary(library);
    }
    void initialize(bool with_runtime = true) {
        if (with_runtime) session.initialize();
        library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) throw std::runtime_error("Vulkan loader unavailable");
        const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
        if (!gipa) throw std::runtime_error("Vulkan resolver unavailable");
        const auto instance_extensions = with_runtime ? session.instance_extensions() : std::vector<std::string>{};
        const auto instance_names = names(instance_extensions);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "x4vr-submission-diagnostic"; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = static_cast<uint32_t>(instance_names.size()); ci.ppEnabledExtensionNames = instance_names.data();
        const auto create = resolve<PFN_vkCreateInstance>(gipa, VkInstance{}, "vkCreateInstance");
        check(create(&ci, nullptr, &instance), "create compositor-compatible instance");
        DestroyInstance = resolve<PFN_vkDestroyInstance>(gipa, instance, "vkDestroyInstance");
#define INSTANCE_FN(name) const auto name = resolve<PFN_vk##name>(gipa, instance, "vk" #name)
        INSTANCE_FN(GetPhysicalDeviceProperties);
        INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties); INSTANCE_FN(CreateDevice); INSTANCE_FN(GetDeviceProcAddr);
#undef INSTANCE_FN
        if (with_runtime) physical = session.output_device(instance);
        else {
            const auto enumerate = resolve<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
            uint32_t count{}; check(enumerate(instance, &count, nullptr), "enumerate GPUs");
            if (!count) throw std::runtime_error("No Vulkan GPU");
            std::vector<VkPhysicalDevice> devices(count);
            check(enumerate(instance, &count, devices.data()), "enumerate GPU handles");
            physical = devices[0];
        }
        VkPhysicalDeviceProperties properties{}; GetPhysicalDeviceProperties(physical, &properties);
        std::cout << "gpu=" << properties.deviceName;
        if (with_runtime) std::cout << " headset=" << session.headset_model();
        std::cout << std::endl;
        uint32_t count{}; GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        if (family == count) throw std::runtime_error("No graphics queue on headset GPU");
        const auto device_extensions = with_runtime ? session.device_extensions(physical) : std::vector<std::string>{};
        const auto device_names = names(device_extensions);
        const float priority = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(device_names.size()); dci.ppEnabledExtensionNames = device_names.data();
        check(CreateDevice(physical, &dci, nullptr, &device), "create compositor-compatible device");
#define RESOLVE(name) name = resolve<PFN_vk##name>(GetDeviceProcAddr, device, "vk" #name);
        DEVICE_FUNCTIONS(RESOLVE)
#undef RESOLVE
        GetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = family;
        check(CreateCommandPool(device, &pci, nullptr, &pool), "create command pool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        check(AllocateCommandBuffers(device, &cai, &command), "allocate command buffer");
    }
    void create_images(const x4vr::Frame& frame) {
        width = frame.width; height = frame.height;
        const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
        const x4vr::EyeTargetContext context{device, physical,
            resolve<PFN_vkGetDeviceProcAddr>(gipa, instance, "vkGetDeviceProcAddr"),
            resolve<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa, instance, "vkGetPhysicalDeviceMemoryProperties"),
            resolve<PFN_vkGetPhysicalDeviceImageFormatProperties>(gipa, instance, "vkGetPhysicalDeviceImageFormatProperties")};
        targets = x4vr::EyeTargets::create(context, {width,height}, format);
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = format; attachments[1].format = targets->depth_format();
        for (auto& attachment : attachments) {
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color; subpass.pDepthStencilAttachment = &depth;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2; pass.pAttachments = attachments; pass.subpassCount = 1; pass.pSubpasses = &subpass;
        check(CreateRenderPass(device, &pass, nullptr, &render_pass), "create eye render pass");
        for (unsigned i = 0; i < 2; ++i) {
            const VkImageView views[]{targets->eyes()[i].color.view, targets->eyes()[i].depth.view};
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = render_pass; framebuffer.attachmentCount = 2; framebuffer.pAttachments = views;
            framebuffer.width = width; framebuffer.height = height; framebuffer.layers = 1;
            check(CreateFramebuffer(device, &framebuffer, nullptr, &framebuffers[i]), "create eye framebuffer");
        }
    }
    void submit_gray(const x4vr::Frame& frame, bool initialized, bool with_runtime = true) {
        if (frame.width != width || frame.height != height) throw std::runtime_error("Runtime extent changed; restart diagnostic");
        // One owner thread serializes every queue/compositor operation. This is a
        // correctness test, deliberately using idle waits, not performance code.
        check(QueueWaitIdle(queue), "wait for previous compositor copy");
        check(ResetCommandPool(device, pool, 0), "reset command pool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(BeginCommandBuffer(command, &begin), "begin eye clears");
        for (unsigned i = 0; i < 2; ++i) {
            const auto image = targets->eyes()[i].color.image;
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcAccessMask = initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            CmdPipelineBarrier(command, initialized ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            auto depth_barrier = barrier;
            depth_barrier.image = targets->eyes()[i].depth.image;
            depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth_barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_barrier.srcAccessMask = initialized ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0;
            depth_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            constexpr auto depth_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            CmdPipelineBarrier(command, initialized ? depth_stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                depth_stages, 0, 0, nullptr, 0, nullptr, 1, &depth_barrier);
            const float shade = with_runtime || i == 0 ? 0.04f : 0.08f;
            VkClearValue clear[2]{}; clear[0].color = {{shade,shade,shade,1.f}};
            clear[1].depthStencil = {with_runtime || i == 0 ? 0.f : 0.5f,0};
            VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            pass.renderPass = render_pass; pass.framebuffer = framebuffers[i];
            pass.renderArea.extent = {width,height}; pass.clearValueCount = 2; pass.pClearValues = clear;
            CmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
            CmdEndRenderPass(command);
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        check(EndCommandBuffer(command), "end eye clears");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        check(QueueSubmit(queue, 1, &submit, nullptr), "submit eye clears");
        std::array<vr::VRVulkanTextureData_t, 2> textures{};
        for (unsigned i = 0; i < 2; ++i) {
            textures[i] = {reinterpret_cast<uint64_t>(targets->eyes()[i].color.image), device, physical, instance, queue,
                family, width, height, static_cast<uint32_t>(format), 1};
        }
        if (with_runtime) {
            session.submit(frame.id, textures, vr::ColorSpace_Auto);
            session.post_present();
        }
    }
    void verify_target_pixels() {
        // Standalone diagnostic only: one pixel from each color/depth image,
        // after all render work completes. No game memory or headset access.
        if (format != VK_FORMAT_R8G8B8A8_UNORM || targets->depth_format() != VK_FORMAT_D32_SFLOAT)
            throw std::runtime_error("Readback fixture expects RGBA8/D32F");
        check(QueueWaitIdle(queue), "complete renders before readback");
        VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{};
        const auto cleanup = [&] {
            if (mapped) UnmapMemory(device, memory);
            if (buffer) DestroyBuffer(device, buffer, nullptr);
            if (memory) FreeMemory(device, memory, nullptr);
        };
        try {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = 16;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            check(CreateBuffer(device, &info, nullptr, &buffer), "create readback buffer");
            VkMemoryRequirements requirements{}; GetBufferMemoryRequirements(device, buffer, &requirements);
            VkPhysicalDeviceMemoryProperties properties{};
            const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
            resolve<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa, instance, "vkGetPhysicalDeviceMemoryProperties")(physical, &properties);
            const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            uint32_t type = 0;
            while (type < properties.memoryTypeCount && (!(requirements.memoryTypeBits & (1u<<type)) ||
                   (properties.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
            if (type == properties.memoryTypeCount) throw std::runtime_error("No coherent readback memory");
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
            check(AllocateMemory(device, &allocation, nullptr, &memory), "allocate readback memory");
            check(BindBufferMemory(device, buffer, memory, 0), "bind readback memory");
            check(ResetCommandPool(device, pool, 0), "reset readback commands");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            check(BeginCommandBuffer(command, &begin), "begin readback commands");
            for (unsigned i = 0; i < 2; ++i) {
                VkBufferImageCopy copy{}; copy.bufferOffset = i*4;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {1,1,1};
                CmdCopyImageToBuffer(command, targets->eyes()[i].color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                VkImageMemoryBarrier depth{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                depth.image = targets->eyes()[i].depth.image; depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
                depth.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; depth.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                depth.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; depth.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                depth.srcQueueFamilyIndex = depth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                constexpr auto depth_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                CmdPipelineBarrier(command, depth_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &depth);
                copy.bufferOffset = 8+i*4; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                CmdCopyImageToBuffer(command, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                depth.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                depth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, depth_stages, 0, 0, nullptr, 0, nullptr, 1, &depth);
            }
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
            check(EndCommandBuffer(command), "end readback commands");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            check(QueueSubmit(queue, 1, &submit, nullptr), "submit readback");
            check(QueueWaitIdle(queue), "complete readback");
            check(MapMemory(device, memory, 0, 16, 0, &mapped), "map readback");
            const auto* bytes = static_cast<const unsigned char*>(mapped);
            const unsigned char expected[]{10,10,10,255,20,20,20,255};
            float depths[2]{}; std::memcpy(depths, bytes+8, sizeof(depths));
            if (std::memcmp(bytes, expected, sizeof(expected)) || depths[0] != 0.f || depths[1] != 0.5f)
                throw std::runtime_error("Eye target readback differs from independent clear values");
            std::cout << "readback: left RGBA=(10,10,10,255) depth=0; right RGBA=(20,20,20,255) depth=0.5" << std::endl;
        } catch (...) { QueueWaitIdle(queue); cleanup(); throw; }
        cleanup();
    }
};
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--targets-only") {
        try {
            Probe probe; probe.initialize(false);
            x4vr::Frame frame; frame.width = 3292; frame.height = 2820;
            probe.create_images(frame);
            probe.submit_gray(frame, false, false);
            probe.submit_gray(frame, true, false);
            check(probe.QueueWaitIdle(probe.queue), "complete target render passes");
            probe.verify_target_pixels();
            std::cout << "Two distinct color/depth eye targets: render-pass clears, reuse and completion passed (no OpenVR)." << std::endl;
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
    }
    if ((argc != 2 && argc != 4) || std::string(argv[1]) != "--submit-gray") {
        std::cout << "Use --submit-gray to submit 90 neutral gray frame pairs.\n"
            "Optional: --format rgba8|bgra8|rgba8-srgb|bgra8-srgb|rgba16f\n"
            "Use --targets-only for a GPU attachment/render-pass test without starting OpenVR.\n"
            "Only --submit-gray takes SteamVR scene focus. Neither mode tests scene geometry, head tracking visuals or X4 rendering.\n";
        return argc == 1 ? 0 : 2;
    }
    try {
        Probe probe;
        if (argc == 4) {
            if (std::string(argv[2]) != "--format") throw std::invalid_argument("Expected --format");
            const std::string format = argv[3];
            if (format == "rgba8") probe.format = VK_FORMAT_R8G8B8A8_UNORM;
            else if (format == "bgra8") probe.format = VK_FORMAT_B8G8R8A8_UNORM;
            else if (format == "rgba8-srgb") probe.format = VK_FORMAT_R8G8B8A8_SRGB;
            else if (format == "bgra8-srgb") probe.format = VK_FORMAT_B8G8R8A8_SRGB;
            else if (format == "rgba16f") probe.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            else throw std::invalid_argument("Unknown format");
        }
        probe.initialize();
        std::cout << "vk_format=" << probe.format << std::endl;
        unsigned submitted = 0;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while (submitted < 90 && std::chrono::steady_clock::now() < deadline) {
            x4vr::Frame frame;
            const auto status = probe.session.begin_frame(frame, 0.1f, 1000);
            if (status == x4vr::FrameStatus::quit_requested) throw std::runtime_error("Runtime requested quit");
            if (status != x4vr::FrameStatus::ready) { Sleep(10); continue; }
            if (!submitted) { probe.create_images(frame); std::cout << "eye_extent=" << frame.width << 'x' << frame.height << std::endl; }
            probe.submit_gray(frame, submitted != 0); ++submitted;
        }
        if (submitted != 90) throw std::runtime_error("Submission deadline reached: "+probe.session.last_error());
        check(probe.QueueWaitIdle(probe.queue), "wait for final compositor copy");
        std::cout << "submitted_frame_pairs=" << submitted << " (not visual verification)" << std::endl;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}
