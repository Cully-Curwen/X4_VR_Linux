// Backend smoke test without X4: drives RuntimeBootstrap the way the Vulkan layer does (runtime
// extensions, headset GPU, a second queue for submission, session start, stereo frames with each
// eye's head pose, theater screen, cursor, session end). X4VR_RUNTIME=openxr picks OpenXR, and
// XR_RUNTIME_JSON a specific OpenXR runtime. Usage: runtime_smoke [seconds]
#include <windows.h>
#include <vulkan/vulkan.h>
#include <x4vr/runtime_bootstrap.hpp>
#include <x4vr/eye_targets.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what)+" failed: "+std::to_string(result));
}
void check(const std::string& error, const char* what) {
    if (!error.empty()) throw std::runtime_error(std::string(what)+": "+error);
}
std::vector<const char*> pointers(const std::vector<std::string>& strings) {
    std::vector<const char*> result;
    for (const auto& s : strings) result.push_back(s.c_str());
    return result;
}
}

int main(int argc, char** argv) try {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10;
    char backend[16] = "openvr";
    GetEnvironmentVariableA("X4VR_RUNTIME", backend, sizeof(backend));
    std::cout << "X4VR_RUNTIME=" << backend << '\n';
    const auto runtime = x4vr::acquire_runtime_bootstrap();
    const auto vulkan = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!vulkan) throw std::runtime_error("Vulkan loader unavailable");
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    const auto instance_extensions = runtime->instance_extensions();
    const auto instance_names = pointers(instance_extensions);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "x4vr-runtime-smoke"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(instance_names.size()); ici.ppEnabledExtensionNames = instance_names.data();
    VkInstance instance{};
    check(reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance), "vkCreateInstance");
#define INSTANCE_FN(name) const auto name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name))
    INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties); INSTANCE_FN(CreateDevice); INSTANCE_FN(GetDeviceProcAddr);
    INSTANCE_FN(GetPhysicalDeviceMemoryProperties); INSTANCE_FN(GetPhysicalDeviceImageFormatProperties);
#undef INSTANCE_FN
    const auto physical = runtime->output_device(instance);
    uint32_t count{};
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count || families[family].queueCount < 2) throw std::runtime_error("need two graphics queues (game + layer)");
    const auto device_extensions = runtime->device_extensions(physical);
    const auto device_names = pointers(device_extensions);
    const float priorities[2]{1, 1};
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 2; qci.pQueuePriorities = priorities;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(device_names.size()); dci.ppEnabledExtensionNames = device_names.data();
    VkDevice device{};
    check(CreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
#define DEVICE_FN(name) const auto name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name))
    DEVICE_FN(GetDeviceQueue); DEVICE_FN(CreateCommandPool); DEVICE_FN(AllocateCommandBuffers); DEVICE_FN(BeginCommandBuffer);
    DEVICE_FN(EndCommandBuffer); DEVICE_FN(CmdPipelineBarrier); DEVICE_FN(CmdClearColorImage); DEVICE_FN(QueueSubmit);
    DEVICE_FN(QueueWaitIdle); DEVICE_FN(DeviceWaitIdle); DEVICE_FN(DestroyCommandPool); DEVICE_FN(DestroyDevice);
#undef DEVICE_FN
    VkQueue game{}, layer{};
    GetDeviceQueue(device, family, 0, &game); GetDeviceQueue(device, family, 1, &layer);

    // "Game" eye images like the layer's ring: X4's B8G8R8A8_UNORM, filled, left in TRANSFER_SRC_OPTIMAL.
    const VkExtent2D extent{2048, 2048};
    auto targets = x4vr::EyeTargets::create({device, physical, GetDeviceProcAddr, GetPhysicalDeviceMemoryProperties,
                                             GetPhysicalDeviceImageFormatProperties}, extent, VK_FORMAT_B8G8R8A8_UNORM);
    VkCommandPool pool{};
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = family;
    check(CreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer command{};
    check(AllocateCommandBuffers(device, &cai, &command), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    for (int e = 0; e < 2; ++e) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = targets->eyes()[e].color.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        const VkClearColorValue color = e ? VkClearColorValue{{0.2f, 0.1f, 0.05f, 1}} : VkClearColorValue{{0.05f, 0.2f, 0.1f, 1}}; // BGRA
        CmdClearColorImage(command, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    check(EndCommandBuffer(command), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &command;
    check(QueueSubmit(game, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    check(QueueWaitIdle(game), "vkQueueWaitIdle");

    x4vr::XrVulkanContext context{instance, physical, physical, device, layer, family, 1,
        reinterpret_cast<decltype(x4vr::XrVulkanContext::gipa)>(gipa), reinterpret_cast<decltype(x4vr::XrVulkanContext::gdpa)>(GetDeviceProcAddr), nullptr};
    check(runtime->start_session(context), "start_session");
    const auto eyes = runtime->eye_setup();
    // The layer's padded texture: symmetric span around the view axis, each eye's frustum cropped.
    float span_x = 0, span_y = 0;
    for (const auto& t : eyes.tangents) {
        span_x = std::max({span_x, std::fabs(t[0]), std::fabs(t[1])});
        span_y = std::max({span_y, std::fabs(t[2]), std::fabs(t[3])});
    }
    const bool valve = !runtime->openxr();
    std::array<vr::VRTextureBounds_t, 2> bounds{};
    std::array<vr::VRVulkanTextureData_t, 2> textures{};
    for (int e = 0; e < 2; ++e) {
        const auto& t = eyes.tangents[e];
        std::cout << "eye" << e << " x=" << eyes.head_from_eye[e].m[0][3] << " tangents " << t[0] << ' ' << t[1] << ' ' << t[2] << ' ' << t[3] << '\n';
        bounds[e] = valve ? vr::VRTextureBounds_t{0.5f+0.5f*t[0]/span_x, 0.5f-0.5f*t[3]/span_y, 0.5f+0.5f*t[1]/span_x, 0.5f-0.5f*t[2]/span_y}
                          : vr::VRTextureBounds_t{0.5f+0.5f*t[0]/span_x, 0.5f+0.5f*t[2]/span_y, 0.5f+0.5f*t[1]/span_x, 0.5f+0.5f*t[3]/span_y};
        auto& v = textures[e];
        v.m_nImage = reinterpret_cast<uint64_t>(targets->eyes()[e].color.image);
        v.m_pDevice = device; v.m_pPhysicalDevice = physical; v.m_pInstance = instance; v.m_pQueue = layer; v.m_nQueueFamilyIndex = family;
        v.m_nWidth = extent.width; v.m_nHeight = extent.height; v.m_nFormat = VK_FORMAT_B8G8R8A8_UNORM; v.m_nSampleCount = 1;
    }
    std::vector<uint8_t> arrow(32*32*4, 0); // a white square cursor, opaque in its top-left quarter
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) for (int c = 0; c < 4; ++c) arrow[(y*32+x)*4+c] = 255;

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    uint64_t frames = 0, errors = 0, tracked = 0;
    std::string last_error;
    bool cursor_sent = false;
    std::array<double, 3> wait_ms{}, submit_ms{}; std::array<uint64_t, 3> phase_frames{}; // per third: stereo, +cursor, +theater
    const auto ms = [](clock::time_point from) { return std::chrono::duration<double, std::milli>(clock::now()-from).count(); };
    while (std::chrono::duration<double>(clock::now()-start).count() < seconds) {
        const double t = std::chrono::duration<double>(clock::now()-start).count();
        const int phase = std::min(2, int(3*t/seconds));
        const auto waiting = clock::now();
        auto error = runtime->wait_frame();
        wait_ms[phase] += ms(waiting);
        const auto working = clock::now();
        if (error.empty()) {
            x4vr::Matrix head;
            const bool ok = runtime->predicted_tracking(head, 0.035f) == x4vr::FrameStatus::ready;
            tracked += ok;
            if (!ok) head = x4vr::Matrix::identity();
            const bool theater = t > seconds*2/3;
            if (theater) {
                auto screen = x4vr::Matrix::identity(); screen.m[2][3] = -2;
                error = runtime->show_theater(&textures[0], {0.25f, 0.25f, 0.75f, 0.75f}, screen, 2.2f);
            }
            if (error.empty() && t > seconds/3) {
                auto at = x4vr::Matrix::identity(); at.m[2][3] = -1.5f; // head-locked, 1.5 m ahead
                error = runtime->show_cursor(cursor_sent ? nullptr : arrow.data(), 32, 32, false, at, 0.05f);
                cursor_sent = true;
            }
            if (error.empty()) error = runtime->submit_frame(textures, bounds, {head, head}, true);
        }
        submit_ms[phase] += ms(working); ++phase_frames[phase];
        if (error.empty()) ++frames;
        else { ++errors; last_error = error; Sleep(5); }
    }
    runtime->hide_cursor(); runtime->hide_theater();
    check(DeviceWaitIdle(device), "vkDeviceWaitIdle");
    runtime->end_session(device);
    targets.reset();
    DestroyCommandPool(device, pool, nullptr);
    DestroyDevice(device, nullptr);
    std::cout << "frames=" << frames << " fps=" << double(frames)/seconds << " tracked=" << tracked << " errors=" << errors
              << (last_error.empty() ? "" : " last_error="+last_error) << '\n';
    for (int i = 0; i < 3; ++i)
        std::cout << "phase" << i << " fps=" << 3*double(phase_frames[i])/seconds << " wait_ms=" << wait_ms[i]/double(std::max<uint64_t>(phase_frames[i], 1))
                  << " work_ms=" << submit_ms[i]/double(std::max<uint64_t>(phase_frames[i], 1)) << '\n';
    std::cout << "RESULT " << (frames > seconds*60 && tracked == frames && !errors ? "PASS" : "CHECK") << '\n';
    return 0; // the runtime stays pinned for the process lifetime, as in X4
} catch (const std::exception& error) {
    std::cout << "RESULT FAIL: " << error.what() << '\n';
    return 1;
}
