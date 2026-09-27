#define NOMINMAX
#include "openxr_runtime.hpp"
#include <windows.h>
#include <unknwn.h> // openxr_platform.h's Win32 part needs IUnknown
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace x4vr {
XrPosef to_xr(const Matrix& pose) {
    const auto& m = pose.m;
    float w, x, y, z;
    const float trace = m[0][0]+m[1][1]+m[2][2];
    if (trace > 0) {
        const float s = std::sqrt(trace+1)*2;
        w = s/4; x = (m[2][1]-m[1][2])/s; y = (m[0][2]-m[2][0])/s; z = (m[1][0]-m[0][1])/s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1+m[0][0]-m[1][1]-m[2][2])*2;
        w = (m[2][1]-m[1][2])/s; x = s/4; y = (m[0][1]+m[1][0])/s; z = (m[0][2]+m[2][0])/s;
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1+m[1][1]-m[0][0]-m[2][2])*2;
        w = (m[0][2]-m[2][0])/s; x = (m[0][1]+m[1][0])/s; y = s/4; z = (m[1][2]+m[2][1])/s;
    } else {
        const float s = std::sqrt(1+m[2][2]-m[0][0]-m[1][1])*2;
        w = (m[1][0]-m[0][1])/s; x = (m[0][2]+m[2][0])/s; y = (m[1][2]+m[2][1])/s; z = s/4;
    }
    const float n = std::sqrt(w*w+x*x+y*y+z*z);
    return {{x/n, y/n, z/n, w/n}, {m[0][3], m[1][3], m[2][3]}};
}
Matrix from_xr(const XrPosef& pose) {
    const auto& q = pose.orientation;
    auto r = Matrix::identity();
    r.m[0][0] = 1-2*(q.y*q.y+q.z*q.z); r.m[0][1] = 2*(q.x*q.y-q.z*q.w);   r.m[0][2] = 2*(q.x*q.z+q.y*q.w);
    r.m[1][0] = 2*(q.x*q.y+q.z*q.w);   r.m[1][1] = 1-2*(q.x*q.x+q.z*q.z); r.m[1][2] = 2*(q.y*q.z-q.x*q.w);
    r.m[2][0] = 2*(q.x*q.z-q.y*q.w);   r.m[2][1] = 2*(q.y*q.z+q.x*q.w);   r.m[2][2] = 1-2*(q.x*q.x+q.y*q.y);
    r.m[0][3] = pose.position.x; r.m[1][3] = pose.position.y; r.m[2][3] = pose.position.z;
    return r;
}
XrFovf fov_from_tangents(const std::array<float, 4>& t) {
    return {std::atan(t[0]), std::atan(t[1]), std::atan(-t[2]), std::atan(-t[3])};
}
std::array<float, 4> tangents_from_fov(const XrFovf& f) {
    return {std::tan(f.angleLeft), std::tan(f.angleRight), -std::tan(f.angleUp), -std::tan(f.angleDown)};
}
XrRect2Di bounds_rect(const vr::VRTextureBounds_t& b, uint32_t width, uint32_t height) {
    const auto px = [](float f, uint32_t size) { return int32_t(std::lround(std::clamp(f, 0.f, 1.f)*float(size))); };
    const int32_t x0 = px(std::min(b.uMin, b.uMax), width), x1 = px(std::max(b.uMin, b.uMax), width);
    const int32_t y0 = px(std::min(b.vMin, b.vMax), height), y1 = px(std::max(b.vMin, b.vMax), height);
    return {{x0, y0}, {x1-x0, y1-y0}};
}
int64_t swapchain_format(int64_t source, const std::vector<int64_t>& offered) {
    const auto has = [&](int64_t f) { return std::find(offered.begin(), offered.end(), f) != offered.end(); };
    const int64_t srgb = source == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB
                       : source == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : source;
    return has(srgb) ? srgb : has(source) ? source : 0;
}

namespace {
void check(XrInstance instance, XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) return;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (instance) xrResultToString(instance, result, name);
    throw std::runtime_error(std::string(what)+" failed: OpenXR "+name+" ("+std::to_string(result)+")");
}
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what)+" failed: VkResult "+std::to_string(result));
}
std::vector<std::string> words(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> result;
    for (std::string w; in >> w;) result.push_back(w);
    return result;
}
// Command slots, one per swapchain written per frame: each waits for its previous use.
enum Slot { left_eye, right_eye, theater_slot, cursor_slot, slot_count };
}

struct OpenXRRuntime::State {
    XrInstance instance{};
    XrSystemId system{};
    PFN_xrGetVulkanInstanceExtensionsKHR instance_extensions{};
    PFN_xrGetVulkanDeviceExtensionsKHR device_extensions{};
    PFN_xrGetVulkanGraphicsDeviceKHR graphics_device{};
    PFN_xrConvertWin32PerformanceCounterToTimeKHR to_time{};
    std::mutex mutex; // session lifetime, frame loop and layers; pose queries don't take it
    XrSession session{};
    XrSpace local{}, view{}; // LOCAL plays OpenVR's seated universe
    std::atomic_bool ready{}; // session and spaces exist, eye setup known
    bool running{}, begun{}, lost{};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    RuntimeBootstrap::EyeSetup eyes{};
    std::vector<int64_t> formats;
    struct Chain { XrSwapchain handle{}; std::vector<VkImage> images; uint32_t width{}, height{}; int64_t format{}; };
    std::array<Chain, 2> eye_chains;
    Chain theater, cursor;
    bool theater_shown{}, cursor_shown{}, cursor_on_screen{};
    Matrix theater_pose, cursor_pose;
    float theater_width{}, cursor_width{};
    XrVulkanContext vk{};
    VkCommandPool pool{};
    std::array<VkCommandBuffer, slot_count> commands{};
    std::array<VkFence, slot_count> fences{};
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    void* staging_data{};
    VkDeviceSize staging_size{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
#define XR_VK_FUNCTIONS(F) F(CreateCommandPool) F(DestroyCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) \
    F(EndCommandBuffer) F(CmdPipelineBarrier) F(CmdCopyImage) F(CmdCopyBufferToImage) F(QueueSubmit) F(CreateFence) \
    F(DestroyFence) F(WaitForFences) F(ResetFences) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) \
    F(AllocateMemory) F(FreeMemory) F(BindBufferMemory) F(MapMemory)
#define MEMBER(name) PFN_vk##name name{};
    XR_VK_FUNCTIONS(MEMBER)
#undef MEMBER

    void xr(XrResult result, const char* what) const { check(instance, result, what); }
    // Runtime events: session start/stop and loss.
    void pump() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xr(xrBeginSession(session, &begin), "xrBeginSession");
                    running = true;
                    OutputDebugStringA("X4VR OpenXR: session running\n");
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    running = begun = false;
                    xr(xrEndSession(session), "xrEndSession");
                    OutputDebugStringA("X4VR OpenXR: session stopped\n");
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    running = begun = false; lost = true;
                    OutputDebugStringA("X4VR OpenXR: session ended by the runtime\n");
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                running = begun = false; lost = true;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }
    XrTime now(double ahead_seconds = 0) const {
        LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
        XrTime time{};
        xr(to_time(instance, &counter, &time), "xrConvertWin32PerformanceCounterToTimeKHR");
        return time + XrTime(ahead_seconds*1e9);
    }
    void wait_slot(Slot slot) {
        check(WaitForFences(vk.device, 1, &fences[slot], VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }
    void ensure(Chain& c, Slot slot, uint32_t width, uint32_t height, int64_t format) {
        if (!format) throw std::runtime_error("OpenXR runtime offers no swapchain format for the game image");
        if (c.handle && c.width == width && c.height == height && c.format == format) return;
        if (c.handle) { wait_slot(slot); xrDestroySwapchain(c.handle); c = {}; }
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format; ci.sampleCount = 1; ci.width = width; ci.height = height;
        ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        xr(xrCreateSwapchain(session, &ci, &c.handle), "xrCreateSwapchain");
        uint32_t count{};
        xr(xrEnumerateSwapchainImages(c.handle, 0, &count, nullptr), "xrEnumerateSwapchainImages");
        std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xr(xrEnumerateSwapchainImages(c.handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
           "xrEnumerateSwapchainImages");
        for (const auto& image : images) c.images.push_back(image.image);
        c.width = width; c.height = height; c.format = format;
    }
    // Next swapchain image: record(command, image) writes it in TRANSFER_DST layout; submitted on
    // the session's queue, then released (the runtime orders its reads after that submission).
    template<class Record> void write(Chain& c, Slot slot, Record record) {
        wait_slot(slot);
        check(ResetFences(vk.device, 1, &fences[slot]), "vkResetFences");
        uint32_t index{};
        xr(xrAcquireSwapchainImage(c.handle, nullptr, &index), "xrAcquireSwapchainImage");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout = XR_INFINITE_DURATION;
        xr(xrWaitSwapchainImage(c.handle, &wait), "xrWaitSwapchainImage");
        const auto command = commands[slot];
        const auto image = c.images[index];
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; // fully overwritten
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        record(command, image);
        // The OpenXR Vulkan binding hands color swapchain images back in COLOR_ATTACHMENT_OPTIMAL.
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        check(EndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        check(QueueSubmit(vk.queue, 1, &submit, fences[slot]), "vkQueueSubmit");
        xr(xrReleaseSwapchainImage(c.handle, nullptr), "xrReleaseSwapchainImage");
    }
    // Copy `rect` of a game-side image (TRANSFER_SRC_OPTIMAL) into the chain's next image.
    void copy(Chain& c, Slot slot, const vr::VRVulkanTextureData_t& source, XrRect2Di rect) {
        write(c, slot, [&](VkCommandBuffer command, VkImage image) {
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffset = {rect.offset.x, rect.offset.y, 0};
            region.extent = {uint32_t(rect.extent.width), uint32_t(rect.extent.height), 1};
            CmdCopyImage(command, reinterpret_cast<VkImage>(source.m_nImage), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });
    }
    void ensure_staging(VkDeviceSize size) {
        if (staging_size >= size) return;
        if (staging) { wait_slot(cursor_slot); DestroyBuffer(vk.device, staging, nullptr); FreeMemory(vk.device, staging_memory, nullptr); }
        staging = {}; staging_memory = {}; staging_data = nullptr; staging_size = 0;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(CreateBuffer(vk.device, &info, nullptr, &staging), "vkCreateBuffer");
        VkMemoryRequirements need{};
        GetBufferMemoryRequirements(vk.device, staging, &need);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = need.size; allocate.memoryTypeIndex = UINT32_MAX;
        const auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount && allocate.memoryTypeIndex == UINT32_MAX; ++i)
            if ((need.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) allocate.memoryTypeIndex = i;
        if (allocate.memoryTypeIndex == UINT32_MAX) throw std::runtime_error("no host-visible memory for the cursor");
        check(AllocateMemory(vk.device, &allocate, nullptr, &staging_memory), "vkAllocateMemory");
        check(BindBufferMemory(vk.device, staging, staging_memory, 0), "vkBindBufferMemory");
        check(MapMemory(vk.device, staging_memory, 0, VK_WHOLE_SIZE, 0, &staging_data), "vkMapMemory");
        staging_size = size;
    }
};

OpenXRRuntime::OpenXRRuntime() : s_(std::make_unique<State>()) {
    auto& s = *s_;
    uint32_t count{};
    check(nullptr, xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties");
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    check(nullptr, xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()), "xrEnumerateInstanceExtensionProperties");
    const char* const enabled[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
    for (const auto* name : enabled)
        if (std::none_of(available.begin(), available.end(), [&](const auto& e) { return !std::strcmp(e.extensionName, name); }))
            throw std::runtime_error(std::string("OpenXR runtime lacks ")+name);
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "X4 VR");
    strcpy_s(ci.applicationInfo.engineName, "x4vr");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = uint32_t(std::size(enabled)); ci.enabledExtensionNames = enabled;
    check(nullptr, xrCreateInstance(&ci, &s.instance), "xrCreateInstance");
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    s.xr(xrGetInstanceProperties(s.instance, &properties), "xrGetInstanceProperties");
    OutputDebugStringA((std::string("X4VR OpenXR: runtime ")+properties.runtimeName+"\n").c_str());
    XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO}; system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    s.xr(xrGetSystem(s.instance, &system, &s.system), "xrGetSystem");
    const auto resolve = [&](const char* name, auto& function) {
        PFN_xrVoidFunction f{};
        s.xr(xrGetInstanceProcAddr(s.instance, name, &f), name);
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(f);
    };
    PFN_xrGetVulkanGraphicsRequirementsKHR requirements{};
    resolve("xrGetVulkanGraphicsRequirementsKHR", requirements);
    resolve("xrGetVulkanInstanceExtensionsKHR", s.instance_extensions);
    resolve("xrGetVulkanDeviceExtensionsKHR", s.device_extensions);
    resolve("xrGetVulkanGraphicsDeviceKHR", s.graphics_device);
    resolve("xrConvertWin32PerformanceCounterToTimeKHR", s.to_time);
    XrGraphicsRequirementsVulkanKHR needs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR}; // must precede xrCreateSession
    s.xr(requirements(s.instance, s.system, &needs), "xrGetVulkanGraphicsRequirementsKHR");
}
OpenXRRuntime::~OpenXRRuntime() {
    // Pinned for the process lifetime like the OpenVR runtime; the session ends with its device.
    if (s_->instance) xrDestroyInstance(s_->instance);
}
std::vector<std::string> OpenXRRuntime::instance_extensions() {
    auto& s = *s_;
    uint32_t size{};
    s.xr(s.instance_extensions(s.instance, s.system, 0, &size, nullptr), "xrGetVulkanInstanceExtensionsKHR");
    std::string text(size, '\0');
    s.xr(s.instance_extensions(s.instance, s.system, size, &size, text.data()), "xrGetVulkanInstanceExtensionsKHR");
    return words(text.c_str());
}
std::vector<std::string> OpenXRRuntime::device_extensions() {
    auto& s = *s_;
    uint32_t size{};
    s.xr(s.device_extensions(s.instance, s.system, 0, &size, nullptr), "xrGetVulkanDeviceExtensionsKHR");
    std::string text(size, '\0');
    s.xr(s.device_extensions(s.instance, s.system, size, &size, text.data()), "xrGetVulkanDeviceExtensionsKHR");
    return words(text.c_str());
}
VkPhysicalDevice_T* OpenXRRuntime::output_device(VkInstance_T* instance) {
    VkPhysicalDevice physical{};
    s_->xr(s_->graphics_device(s_->instance, s_->system, instance, &physical), "xrGetVulkanGraphicsDeviceKHR");
    if (!physical) throw std::runtime_error("OpenXR did not identify the headset GPU");
    return physical;
}
std::string OpenXRRuntime::start_session(const XrVulkanContext& vulkan) try {
    auto& s = *s_;
    std::lock_guard lock(s.mutex);
    if (s.session) return {};
    s.vk = vulkan;
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vulkan.gdpa);
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vulkan.gipa);
#define RESOLVE(name) s.name = reinterpret_cast<PFN_vk##name>(gdpa(vulkan.device, "vk" #name)); \
    if (!s.name) throw std::runtime_error("missing vk" #name);
    XR_VK_FUNCTIONS(RESOLVE)
#undef RESOLVE
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(vulkan.instance, "vkGetPhysicalDeviceMemoryProperties"))(
        vulkan.physical, &s.memory_properties);
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vulkan.instance; binding.physicalDevice = vulkan.output_physical; binding.device = vulkan.device;
    binding.queueFamilyIndex = vulkan.family; binding.queueIndex = vulkan.queue_index;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &binding}; ci.systemId = s.system;
    s.xr(xrCreateSession(s.instance, &ci, &s.session), "xrCreateSession");
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    s.xr(xrCreateReferenceSpace(s.session, &space, &s.local), "xrCreateReferenceSpace(LOCAL)");
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    s.xr(xrCreateReferenceSpace(s.session, &space, &s.view), "xrCreateReferenceSpace(VIEW)");
    uint32_t count{};
    s.xr(xrEnumerateSwapchainFormats(s.session, 0, &count, nullptr), "xrEnumerateSwapchainFormats");
    s.formats.resize(count);
    s.xr(xrEnumerateSwapchainFormats(s.session, count, &count, s.formats.data()), "xrEnumerateSwapchainFormats");

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = vulkan.family;
    check(s.CreateCommandPool(vulkan.device, &pool, nullptr, &s.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = s.pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = slot_count;
    check(s.AllocateCommandBuffers(vulkan.device, &allocate, s.commands.data()), "vkAllocateCommandBuffers");
    for (int i = 0; i < slot_count; ++i) {
        // Created below the loader: needs its dispatch pointer like the layer's own command buffers.
        if (vulkan.set_loader_data && vulkan.set_loader_data(vulkan.device, s.commands[i]) != VK_SUCCESS)
            throw std::runtime_error("vkSetDeviceLoaderData failed");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(s.CreateFence(vulkan.device, &fence, nullptr, &s.fences[i]), "vkCreateFence");
    }
    // READY usually arrives at once; the eye setup below needs a running session on some runtimes.
    for (int i = 0; i < 300 && !s.running && !s.lost; ++i) { s.pump(); if (!s.running) Sleep(10); }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; locate.displayTime = s.now(); locate.space = s.view;
    XrViewState state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    s.xr(xrLocateViews(s.session, &locate, &state, 2, &count, views.data()), "xrLocateViews");
    if (count != 2) throw std::runtime_error("OpenXR stereo view configuration expected");
    for (int e = 0; e < 2; ++e) {
        s.eyes.head_from_eye[e] = from_xr(views[e].pose);
        s.eyes.tangents[e] = tangents_from_fov(views[e].fov);
    }
    s.ready = true;
    std::ostringstream log;
    log << "X4VR OpenXR: session started, eyes at x=" << views[0].pose.position.x << ',' << views[1].pose.position.x
        << (s.running ? "" : " (not running yet)") << '\n';
    OutputDebugStringA(log.str().c_str());
    return {};
} catch (const std::exception& error) {
    return error.what();
}
void OpenXRRuntime::end_session(VkDevice_T* device) {
    auto& s = *s_;
    std::lock_guard lock(s.mutex);
    if (!s.session || device != s.vk.device) return;
    // ponytail: a pose query racing this on the game thread is not excluded; X4 destroys its
    // device at exit only. Add a reader lock around the spaces if that ever crashes.
    s.ready = false;
    if (s.pool) s.WaitForFences(s.vk.device, slot_count, s.fences.data(), VK_TRUE, UINT64_MAX);
    xrDestroySession(s.session); // also destroys its spaces and swapchains
    s.session = {}; s.local = s.view = {}; s.eye_chains = {}; s.theater = s.cursor = {};
    s.running = s.begun = s.theater_shown = s.cursor_shown = false;
    for (auto& fence : s.fences) { if (fence) s.DestroyFence(s.vk.device, fence, nullptr); fence = {}; }
    if (s.pool) s.DestroyCommandPool(s.vk.device, s.pool, nullptr);
    s.pool = {};
    if (s.staging) { s.DestroyBuffer(s.vk.device, s.staging, nullptr); s.FreeMemory(s.vk.device, s.staging_memory, nullptr); }
    s.staging = {}; s.staging_memory = {}; s.staging_data = nullptr; s.staging_size = 0;
    OutputDebugStringA("X4VR OpenXR: session destroyed with its device\n");
}
FrameStatus OpenXRRuntime::predicted_tracking(Matrix& tracking_from_head, float seconds) {
    auto& s = *s_;
    tracking_from_head = {};
    if (!s.ready) return FrameStatus::tracking_unavailable;
    try {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(s.view, s.local, s.now(seconds), &location))) return FrameStatus::tracking_unavailable;
        const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((location.locationFlags & valid) != valid) return FrameStatus::tracking_unavailable;
        tracking_from_head = from_xr(location.pose);
        return FrameStatus::ready;
    } catch (const std::exception&) {
        return FrameStatus::tracking_unavailable;
    }
}
RuntimeBootstrap::EyeSetup OpenXRRuntime::eye_setup() {
    return s_->ready ? s_->eyes : RuntimeBootstrap::EyeSetup{}; // written once before ready
}
std::string OpenXRRuntime::wait_frame() try {
    auto& s = *s_;
    std::lock_guard lock(s.mutex);
    if (!s.session) return "OpenXR session not started";
    s.pump();
    if (s.lost) return "OpenXR session ended by the runtime";
    if (!s.running) return "OpenXR session not running";
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    s.frame = {XR_TYPE_FRAME_STATE};
    s.xr(xrWaitFrame(s.session, &wait, &s.frame), "xrWaitFrame");
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    s.xr(xrBeginFrame(s.session, &begin), "xrBeginFrame"); // XR_FRAME_DISCARDED after an unfinished frame: fine
    s.begun = true;
    return {};
} catch (const std::exception& error) {
    return error.what();
}
std::string OpenXRRuntime::submit_frame(const std::array<vr::VRVulkanTextureData_t, 2>& images,
                                        const std::array<vr::VRTextureBounds_t, 2>& bounds, const std::array<Matrix, 2>& poses,
                                        bool with_pose) try {
    auto& s = *s_;
    std::lock_guard lock(s.mutex);
    if (!s.begun) return "OpenXR frame not begun";
    s.begun = false;
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    XrCompositionLayerQuad theater{XR_TYPE_COMPOSITION_LAYER_QUAD}, cursor{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (s.frame.shouldRender) {
        // Theater fills only eye 0: the other eye's pose is unset (all zero), and a zero pose made
        // Varjo reproject the whole right eye (theater screen shifted up and shrunk).
        std::array<XrView, 2> located{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        if (!with_pose || !is_rigid(poses[0]) || !is_rigid(poses[1])) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = s.frame.predictedDisplayTime; locate.space = s.local;
            XrViewState state{XR_TYPE_VIEW_STATE};
            uint32_t count{};
            s.xr(xrLocateViews(s.session, &locate, &state, 2, &count, located.data()), "xrLocateViews");
        }
        // ponytail: both eyes copied every frame (~0.15 ms at Aero size); copy only the eye with a
        // new image if GPU time gets tight (the other swapchain keeps its last released image).
        for (int e = 0; e < 2; ++e) {
            auto& chain = s.eye_chains[e];
            const auto& image = images[e];
            s.ensure(chain, Slot(e), image.m_nWidth, image.m_nHeight, swapchain_format(int64_t(image.m_nFormat), s.formats));
            s.copy(chain, Slot(e), image, {{0, 0}, {int32_t(image.m_nWidth), int32_t(image.m_nHeight)}});
            // Each eye at the head pose its image was rendered with (Submit_TextureWithPose's job).
            const auto pose = with_pose && is_rigid(poses[e]) ? to_xr(multiply(poses[e], s.eyes.head_from_eye[e])) : located[e].pose;
            views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, nullptr, pose,
                        fov_from_tangents(s.eyes.tangents[e]), {chain.handle, bounds_rect(bounds[e], chain.width, chain.height), 0}};
        }
        projection.space = s.local; projection.viewCount = 2; projection.views = views.data();
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
        if (s.theater_shown) {
            theater.space = s.local; theater.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            theater.subImage = {s.theater.handle, {{0, 0}, {int32_t(s.theater.width), int32_t(s.theater.height)}}, 0};
            theater.pose = to_xr(s.theater_pose);
            theater.size = {s.theater_width, s.theater_width*float(s.theater.height)/float(s.theater.width)};
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&theater));
        }
        if (s.cursor_shown) {
            cursor.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            cursor.space = s.cursor_on_screen ? s.local : s.view; cursor.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cursor.subImage = {s.cursor.handle, {{0, 0}, {int32_t(s.cursor.width), int32_t(s.cursor.height)}}, 0};
            cursor.pose = to_xr(s.cursor_pose);
            cursor.size = {s.cursor_width, s.cursor_width*float(s.cursor.height)/float(s.cursor.width)};
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cursor));
        }
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = s.frame.predictedDisplayTime; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = uint32_t(layers.size()); end.layers = layers.data();
    s.xr(xrEndFrame(s.session, &end), "xrEndFrame");
    return {};
} catch (const std::exception& error) {
    return error.what();
}
std::string OpenXRRuntime::show_theater(const vr::VRVulkanTextureData_t* image, const vr::VRTextureBounds_t& bounds,
                                        const Matrix& seated_from_screen, float width) try {
    auto& s = *s_;
    std::lock_guard lock(s.mutex);
    if (!s.session) return "OpenXR session not started";
    s.theater_pose = seated_from_screen; s.theater_width = width;
    if (image) {
        const auto rect = bounds_rect(bounds, image->m_nWidth, image->m_nHeight);
        s.ensure(s.theater, theater_slot, uint32_t(rect.extent.width), uint32_t(rect.extent.height),
                 swapchain_format(int64_t(image->m_nFormat), s.formats));
        s.copy(s.theater, theater_slot, *image, rect);
    }
    s.theater_shown = s.theater.handle != XR_NULL_HANDLE;
    return {};
} catch (const std::exception& error) {
    return error.what();
}
void OpenXRRuntime::hide_theater() { std::lock_guard lock(s_->mutex); s_->theater_shown = false; }
std::string OpenXRRuntime::show_cursor(const uint8_t* rgba, uint32_t width, uint32_t height, bool on_screen,
                                       const Matrix& placement, float width_m) try {
    auto& s = *s_;
    std::lock_guard lock(s.mutex);
    if (!s.session) return "OpenXR session not started";
    s.cursor_on_screen = on_screen; s.cursor_pose = placement; s.cursor_width = width_m;
    if (rgba) {
        s.ensure(s.cursor, cursor_slot, width, height, swapchain_format(VK_FORMAT_R8G8B8A8_UNORM, s.formats));
        const VkDeviceSize bytes = VkDeviceSize(width)*height*4;
        s.ensure_staging(bytes);
        s.wait_slot(cursor_slot); // the previous upload has read the staging buffer
        std::memcpy(s.staging_data, rgba, size_t(bytes));
        s.write(s.cursor, cursor_slot, [&](VkCommandBuffer command, VkImage image) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width, height, 1};
            s.CmdCopyBufferToImage(command, s.staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });
    }
    s.cursor_shown = s.cursor.handle != XR_NULL_HANDLE;
    return {};
} catch (const std::exception& error) {
    return error.what();
}
void OpenXRRuntime::hide_cursor() { std::lock_guard lock(s_->mutex); s_->cursor_shown = false; }
}
