// OpenXR spike, NOT an X4 integration. Checks what the Vulkan layer's architecture needs from an
// OpenXR runtime: XR_KHR_vulkan_enable (v1: extension strings for an instance/device created by
// someone else, as X4 does), the runtime's GPU, a session on a second queue of the graphics family
// (the layer's private queue), per-eye swapchains where only one eye gets a new image per frame
// (alternate-eye rendering), a quad-only frame (theater), and pose queries from another thread
// (the FreeTrack game thread). Usage: openxr_probe [seconds]. Pick a runtime with XR_RUNTIME_JSON,
// e.g. D:\Varjo\varjo-openxr\VarjoOpenXR.json; default is the system's active runtime.
#include <windows.h>
#include <unknwn.h> // openxr_platform.h's Win32 part needs IUnknown, which WIN32_LEAN_AND_MEAN drops
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
XrInstance xr_instance{};
void xr_check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) return;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (xr_instance) xrResultToString(xr_instance, result, name);
    throw std::runtime_error(std::string(what)+" failed: "+name+" ("+std::to_string(result)+")");
}
#define XR(call) xr_check(call, #call)
void vk_check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what)+" failed: VkResult "+std::to_string(result));
}
template<class F> F xr_function(const char* name) {
    PFN_xrVoidFunction f{};
    XR(xrGetInstanceProcAddr(xr_instance, name, &f));
    return reinterpret_cast<F>(f);
}
std::vector<std::string> split(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    return words;
}
std::vector<const char*> pointers(const std::vector<std::string>& strings) {
    std::vector<const char*> result;
    for (const auto& s : strings) result.push_back(s.c_str());
    return result;
}
std::string version(XrVersion v) {
    return std::to_string(XR_VERSION_MAJOR(v))+'.'+std::to_string(XR_VERSION_MINOR(v))+'.'+std::to_string(XR_VERSION_PATCH(v));
}
const char* state_name(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE"; case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED"; case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED"; case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING"; case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}
constexpr XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};

struct Probe {
    HMODULE vulkan{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{}, queue_index{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    XrSystemId system{};
    XrSession session{};
    XrSpace local{}, view{};
    std::array<XrSwapchain, 2> swapchains{};
    std::array<std::vector<XrSwapchainImageVulkanKHR>, 2> images;
    int32_t width{}, height{};
    PFN_vkDestroyInstance DestroyInstance{};
#define DEVICE_FUNCTIONS(F) F(DestroyDevice) F(DeviceWaitIdle) F(GetDeviceQueue) F(CreateCommandPool) \
    F(DestroyCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) F(EndCommandBuffer) \
    F(ResetCommandBuffer) F(CmdPipelineBarrier) F(CmdClearColorImage) F(QueueSubmit) F(CreateFence) \
    F(DestroyFence) F(WaitForFences) F(ResetFences)
#define MEMBER(name) PFN_vk##name name{};
    DEVICE_FUNCTIONS(MEMBER)
#undef MEMBER
    ~Probe() {
        // OpenXR objects go before the Vulkan device they use.
        for (auto s : swapchains) if (s) xrDestroySwapchain(s);
        if (view) xrDestroySpace(view);
        if (local) xrDestroySpace(local);
        if (session) xrDestroySession(session);
        if (xr_instance) xrDestroyInstance(xr_instance);
        if (device && DeviceWaitIdle) DeviceWaitIdle(device);
        if (fence) DestroyFence(device, fence, nullptr);
        if (pool) DestroyCommandPool(device, pool, nullptr);
        if (device) DestroyDevice(device, nullptr);
        if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
        if (vulkan) FreeLibrary(vulkan);
    }

    bool create_instance() {
        uint32_t count{};
        XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
        std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
        XR(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()));
        std::set<std::string> names;
        for (const auto& e : available) names.insert(e.extensionName);
        for (const auto* e : {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
                              XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME})
            std::cout << "extension " << e << '=' << (names.count(e) ? "yes" : "NO") << '\n';
        if (!names.count(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) throw std::runtime_error("runtime lacks XR_KHR_vulkan_enable");
        const bool qpc = names.count(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) != 0;
        std::vector<const char*> enabled{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
        if (qpc) enabled.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ci.applicationInfo.applicationName, "x4vr-openxr-probe");
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = uint32_t(enabled.size()); ci.enabledExtensionNames = enabled.data();
        XR(xrCreateInstance(&ci, &xr_instance));
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        XR(xrGetInstanceProperties(xr_instance, &ip));
        std::cout << "runtime=" << ip.runtimeName << ' ' << version(ip.runtimeVersion) << '\n';
        XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO}; si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR(xrGetSystem(xr_instance, &si, &system));
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        XR(xrGetSystemProperties(xr_instance, system, &sp));
        std::cout << "system=" << sp.systemName << " max_layers=" << sp.graphicsProperties.maxLayerCount << '\n';
        XR(xrEnumerateViewConfigurationViews(xr_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr));
        std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        XR(xrEnumerateViewConfigurationViews(xr_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data()));
        width = int32_t(views[0].recommendedImageRectWidth); height = int32_t(views[0].recommendedImageRectHeight);
        std::cout << "recommended=" << width << 'x' << height << '\n';
        return qpc;
    }

    void create_vulkan() {
        const auto requirements_fn = xr_function<PFN_xrGetVulkanGraphicsRequirementsKHR>("xrGetVulkanGraphicsRequirementsKHR");
        const auto instance_ext_fn = xr_function<PFN_xrGetVulkanInstanceExtensionsKHR>("xrGetVulkanInstanceExtensionsKHR");
        const auto device_ext_fn = xr_function<PFN_xrGetVulkanDeviceExtensionsKHR>("xrGetVulkanDeviceExtensionsKHR");
        const auto device_fn = xr_function<PFN_xrGetVulkanGraphicsDeviceKHR>("xrGetVulkanGraphicsDeviceKHR");
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        XR(requirements_fn(xr_instance, system, &requirements));
        std::cout << "vulkan_api min=" << version(requirements.minApiVersionSupported)
                  << " max=" << version(requirements.maxApiVersionSupported) << '\n';
        auto strings = [&](auto fn, auto... args) {
            uint32_t n{};
            XR(fn(args..., 0, &n, nullptr));
            std::string text(n, '\0');
            XR(fn(args..., n, &n, text.data()));
            return split(text.c_str());
        };
        const auto instance_extensions = strings(instance_ext_fn, xr_instance, system);
        std::cout << "vulkan_instance_extensions=";
        for (const auto& e : instance_extensions) std::cout << e << ' ';
        std::cout << '\n';

        vulkan = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!vulkan) throw std::runtime_error("Vulkan loader unavailable");
        const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
        const auto instance_names = pointers(instance_extensions);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "x4vr-openxr-probe";
        app.apiVersion = VK_API_VERSION_1_1; // ponytail: fixed like X4's side; the layer cannot choose it either
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = uint32_t(instance_names.size()); ci.ppEnabledExtensionNames = instance_names.data();
        vk_check(reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(&ci, nullptr, &instance), "vkCreateInstance");
#define INSTANCE_FN(name) const auto name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name))
        INSTANCE_FN(EnumeratePhysicalDevices); INSTANCE_FN(GetPhysicalDeviceProperties);
        INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties); INSTANCE_FN(CreateDevice); INSTANCE_FN(GetDeviceProcAddr);
#undef INSTANCE_FN
        DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
        XR(device_fn(xr_instance, system, instance, &physical));
        uint32_t count{};
        vk_check(EnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> gpus(count);
        vk_check(EnumeratePhysicalDevices(instance, &count, gpus.data()), "vkEnumeratePhysicalDevices");
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties p{}; GetPhysicalDeviceProperties(gpus[i], &p);
            std::cout << "gpu" << i << '=' << p.deviceName << (gpus[i] == physical ? "  <- runtime" : "") << '\n';
        }
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        if (family == count) throw std::runtime_error("no graphics queue family");
        // Like the layer: queue 0 stays the game's, the session gets the extra queue when there is one.
        const uint32_t queues = std::min(2u, families[family].queueCount);
        queue_index = queues-1;
        const auto device_extensions = strings(device_ext_fn, xr_instance, system);
        std::cout << "vulkan_device_extensions=";
        for (const auto& e : device_extensions) std::cout << e << ' ';
        std::cout << "\nsession_queue family=" << family << " index=" << queue_index << '\n';
        const auto device_names = pointers(device_extensions);
        const float priorities[2]{1, 1};
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = queues; qci.pQueuePriorities = priorities;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(device_names.size()); dci.ppEnabledExtensionNames = device_names.data();
        vk_check(CreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
#define RESOLVE(name) name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));
        DEVICE_FUNCTIONS(RESOLVE)
#undef RESOLVE
        GetDeviceQueue(device, family, queue_index, &queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = family;
        vk_check(CreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        vk_check(AllocateCommandBuffers(device, &cai, &command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vk_check(CreateFence(device, &fci, nullptr, &fence), "vkCreateFence");
    }

    void create_session() {
        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance = instance; binding.physicalDevice = physical; binding.device = device;
        binding.queueFamilyIndex = family; binding.queueIndex = queue_index;
        XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &binding}; ci.systemId = system;
        XR(xrCreateSession(xr_instance, &ci, &session));
        XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rci.poseInReferenceSpace = identity;
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XR(xrCreateReferenceSpace(session, &rci, &local));
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        XR(xrCreateReferenceSpace(session, &rci, &view));
        uint32_t count{};
        XR(xrEnumerateSwapchainFormats(session, 0, &count, nullptr));
        std::vector<int64_t> formats(count);
        XR(xrEnumerateSwapchainFormats(session, count, &count, formats.data()));
        std::cout << "swapchain_formats=";
        for (auto f : formats) std::cout << f << ' ';
        std::cout << '\n';
        int64_t format = 0;
        for (auto want : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
            if (!format && std::find(formats.begin(), formats.end(), int64_t(want)) != formats.end()) format = want;
        if (!format) throw std::runtime_error("no 8-bit RGBA/BGRA swapchain format");
        std::cout << "swapchain_format=" << format << '\n';
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = format; sci.sampleCount = 1; sci.width = uint32_t(width); sci.height = uint32_t(height);
        sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
        for (int e = 0; e < 2; ++e) {
            XR(xrCreateSwapchain(session, &sci, &swapchains[e]));
            XR(xrEnumerateSwapchainImages(swapchains[e], 0, &count, nullptr));
            images[e].assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            XR(xrEnumerateSwapchainImages(swapchains[e], count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images[e].data())));
        }
        std::cout << "swapchain_images=" << images[0].size() << '\n';
    }

    // Stands in for the layer's ring->swapchain copy: acquire, clear, release.
    void fill(int eye, VkClearColorValue color) {
        uint32_t index{};
        XR(xrAcquireSwapchainImage(swapchains[eye], nullptr, &index));
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        XR(xrWaitSwapchainImage(swapchains[eye], &wi));
        const auto image = images[eye][index].image;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(ResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        vk_check(BeginCommandBuffer(command, &bi), "vkBeginCommandBuffer");
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        CmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
        // OpenXR Vulkan binding: color swapchain images are handed back in COLOR_ATTACHMENT_OPTIMAL.
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vk_check(EndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &command;
        vk_check(QueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
        // ponytail: CPU wait per image; the layer pipelines this with its ring fences.
        vk_check(WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        vk_check(ResetFences(device, 1, &fence), "vkResetFences");
        XR(xrReleaseSwapchainImage(swapchains[eye], nullptr));
    }
};
}

int main(int argc, char** argv) try {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10;
    char runtime_json[MAX_PATH]{};
    std::cout << "XR_RUNTIME_JSON=" << (GetEnvironmentVariableA("XR_RUNTIME_JSON", runtime_json, MAX_PATH) ? runtime_json : "(active runtime)") << '\n';
    Probe p;
    const bool qpc = p.create_instance();
    p.create_vulkan();
    p.create_session();
    PFN_xrConvertWin32PerformanceCounterToTimeKHR to_xr_time{};
    if (qpc) to_xr_time = xr_function<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR");

    // Game-thread stand-in: pose queries at ~250 Hz while the frame loop runs.
    std::atomic_bool running{}, stop{};
    std::atomic<XrTime> last_display{};
    std::atomic_uint64_t pose_ok{}, pose_invalid{}, pose_error{};
    std::thread poser([&] {
        while (!stop) {
            XrTime time = last_display;
            if (to_xr_time) { LARGE_INTEGER now; QueryPerformanceCounter(&now); to_xr_time(xr_instance, &now, &time); time += 35'000'000; }
            if (running && time) {
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                const auto r = xrLocateSpace(p.view, p.local, time, &location);
                const auto valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                ++(XR_FAILED(r) ? pose_error : (location.locationFlags & valid) == valid ? pose_ok : pose_invalid);
            }
            Sleep(4);
        }
    });
    struct Join { std::thread& t; std::atomic_bool& s; ~Join() { s = true; t.join(); } } join{poser, stop};

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto last_wait = start;
    bool exit_requested = false, exiting = false, both_filled = false;
    uint64_t frames = 0, late = 0, projection_frames = 0, quad_frames = 0;
    double interval_sum = 0, interval_max = 0, period_ms = 0, wait_ms = 0, fill_ms = 0, end_ms = 0;
    auto ms_since = [](clock::time_point t) { return std::chrono::duration<double, std::milli>(clock::now()-t).count(); };
    std::array<XrPosef, 2> eye_pose{identity, identity};
    std::array<XrFovf, 2> eye_fov{};
    while (!exiting) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(xr_instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto state = reinterpret_cast<XrEventDataSessionStateChanged&>(event).state;
                std::cout << "state=" << state_name(state) << " t=" << std::chrono::duration<double>(clock::now()-start).count() << '\n';
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR(xrBeginSession(p.session, &bi));
                    running = true;
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    running = false;
                    XR(xrEndSession(p.session));
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) exiting = true;
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) exiting = true;
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        const double elapsed = std::chrono::duration<double>(clock::now()-start).count();
        if (!exit_requested && (elapsed > seconds || (!running && elapsed > seconds+10))) {
            if (running) XR(xrRequestExitSession(p.session)); else exiting = true;
            exit_requested = true;
        }
        if (elapsed > seconds+15) { std::cout << "timeout waiting for EXITING\n"; break; }
        if (!running) { Sleep(10); continue; }

        XrFrameState frame{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        const auto wait_start = clock::now();
        XR(xrWaitFrame(p.session, &wait, &frame));
        wait_ms += ms_since(wait_start);
        const auto now = clock::now();
        period_ms = frame.predictedDisplayPeriod/1e6;
        if (frames) {
            const double ms = std::chrono::duration<double, std::milli>(now-last_wait).count();
            interval_sum += ms; interval_max = std::max(interval_max, ms);
            late += ms > period_ms*1.5;
        }
        last_wait = now;
        last_display = frame.predictedDisplayTime;
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        XR(xrBeginFrame(p.session, &begin));

        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = frame.predictedDisplayTime; li.space = p.local;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        uint32_t count{};
        XR(xrLocateViews(p.session, &li, &vs, 2, &count, views.data()));
        if (frames == 0) for (int e = 0; e < 2; ++e) {
            const auto& f = views[e].fov;
            std::cout << "eye" << e << " fov_deg l=" << f.angleLeft*57.2958f << " r=" << f.angleRight*57.2958f
                      << " u=" << f.angleUp*57.2958f << " d=" << f.angleDown*57.2958f
                      << " pos=" << views[e].pose.position.x << ',' << views[e].pose.position.y << ',' << views[e].pose.position.z << '\n';
        }

        // Last third of the run: theater (quad layer only). Before that: alternate-eye stereo, where
        // only one eye's swapchain gets a new image per frame and the other is resubmitted as is.
        const bool theater = elapsed > seconds*2/3;
        std::vector<const XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::array<XrCompositionLayerProjectionView, 2> projection_views{};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrSwapchainSubImage full0{p.swapchains[0], {{0, 0}, {p.width, p.height}}, 0};
        const auto fill_start = clock::now();
        if (frame.shouldRender) {
            if (theater) {
                p.fill(0, {{0.1f, 0.1f, 0.3f, 1}});
                quad.space = p.local; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH; quad.subImage = full0;
                quad.pose = {{0, 0, 0, 1}, {0, 0, -2}}; quad.size = {2.2f, 2.2f*float(p.height)/float(p.width)};
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
                ++quad_frames;
                both_filled = false; // eye 0's image now holds theater content
            } else {
                for (int e = 0; e < 2; ++e) {
                    if (both_filled && e != int(frames & 1)) continue;
                    p.fill(e, e ? VkClearColorValue{{0.05f, 0.1f, 0.2f, 1}} : VkClearColorValue{{0.1f, 0.2f, 0.05f, 1}});
                    eye_pose[e] = views[e].pose; eye_fov[e] = views[e].fov; // the pose this eye image was "rendered" with
                }
                both_filled = true;
                for (int e = 0; e < 2; ++e)
                    projection_views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, nullptr, eye_pose[e], eye_fov[e],
                                           {p.swapchains[e], {{0, 0}, {p.width, p.height}}, 0}};
                projection.space = p.local; projection.viewCount = 2; projection.views = projection_views.data();
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
                ++projection_frames;
            }
        }
        fill_ms += ms_since(fill_start);
        const auto end_start = clock::now();
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame.predictedDisplayTime; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = uint32_t(layers.size()); end.layers = layers.data();
        XR(xrEndFrame(p.session, &end));
        end_ms += ms_since(end_start);
        ++frames;
    }
    std::cout << "frames=" << frames << " stereo=" << projection_frames << " theater=" << quad_frames
              << " period_ms=" << period_ms << " interval_avg_ms=" << (frames > 1 ? interval_sum/double(frames-1) : 0)
              << " interval_max_ms=" << interval_max << " late=" << late << '\n';
    if (frames) std::cout << "avg_ms wait=" << wait_ms/double(frames) << " fill=" << fill_ms/double(frames)
                          << " end=" << end_ms/double(frames) << '\n';
    std::cout << "pose_thread ok=" << pose_ok << " invalid=" << pose_invalid << " error=" << pose_error
              << " clock=" << (to_xr_time ? "qpc" : "display_time") << '\n';
    std::cout << "RESULT " << (projection_frames && quad_frames && pose_ok && !pose_error ? "PASS" : "INCOMPLETE") << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cout << "RESULT FAIL: " << error.what() << '\n';
    return 1;
}
