#include <windows.h>
#include <vulkan/vk_layer.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include "observe_memory.hpp"
#include "native_camera.hpp"
#include "camera_sampling.hpp"
#include <x4vr/runtime_bootstrap.hpp>
#include <x4vr/vulkan_extensions.hpp>
#include <x4vr/eye_targets.hpp>
#include <array>
#include <cmath>
#include <vector>

#define EXPORT extern "C" __declspec(dllexport)
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetPhysicalDeviceProcAddr(VkInstance, const char*);

namespace {
// Dispatchable objects belonging to an instance/device share their dispatch key.
template<class T> void* key(T object) { return object ? *reinterpret_cast<void**>(object) : nullptr; }
template<class T> uint64_t handle(T value) { return reinterpret_cast<uint64_t>(value); }
struct Instance {
    VkInstance instance{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_GetPhysicalDeviceProcAddr gpdpa{};
    std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
};
struct Device {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::shared_ptr<x4vr::observe::MemoryTracker> memory;
#define DEVICE_FUNCTIONS(F) \
    F(DestroyDevice) F(CreateShaderModule) F(CreateDescriptorSetLayout) F(CreatePipelineLayout) \
    F(CreateGraphicsPipelines) F(CreateComputePipelines) F(CreateSwapchainKHR) F(QueuePresentKHR) \
    F(UpdateDescriptorSets) F(CmdBindDescriptorSets) \
    F(CreateBuffer) F(DestroyBuffer) F(AllocateMemory) F(FreeMemory) \
    F(BindBufferMemory) F(BindBufferMemory2) F(BindBufferMemory2KHR) \
    F(MapMemory) F(UnmapMemory) F(MapMemory2) F(MapMemory2KHR) F(UnmapMemory2) F(UnmapMemory2KHR) \
    F(AllocateDescriptorSets) F(FreeDescriptorSets) F(ResetDescriptorPool) F(DestroyDescriptorPool) \
    F(GetDeviceQueue) F(GetDeviceQueue2)
// Resolved for the VR presenter but not intercepted.
#define PRESENTER_FUNCTIONS(F) \
    F(CreateCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) F(EndCommandBuffer) \
    F(CmdPipelineBarrier) F(CmdCopyImage) F(QueueSubmit) F(CreateSemaphore) F(CreateFence) \
    F(WaitForFences) F(ResetFences) F(GetSwapchainImagesKHR) F(CmdClearColorImage) \
    F(CmdCopyImageToBuffer) F(GetBufferMemoryRequirements)
#define MEMBER(name) PFN_vk##name name{};
    DEVICE_FUNCTIONS(MEMBER)
    PRESENTER_FUNCTIONS(MEMBER)
#undef MEMBER
    VkInstance instance{};
    VkPhysicalDevice physical{};
    PFN_vkSetDeviceLoaderData set_loader_data{};
    PFN_vkGetInstanceProcAddr gipa{};
    std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
};
std::mutex state_mutex;
// Explicit vkDestroyInstance performs runtime shutdown. Never invoke VR_Shutdown
// from CRT/DllMain teardown if the host exits without destroying its instances.
// The OS reclaims this registry on abrupt process exit.
auto& instances = *new std::unordered_map<void*, Instance>;
std::unordered_map<void*, Device> devices;
template<class Object> std::optional<Instance> instance_for(Object o) {
    std::lock_guard lock(state_mutex);
    auto found = instances.find(key(o));
    return found == instances.end() ? std::nullopt : std::optional(found->second);
}
template<class Object> std::optional<Device> device_for(Object o) {
    std::lock_guard lock(state_mutex);
    auto found = devices.find(key(o));
    return found == devices.end() ? std::nullopt : std::optional(found->second);
}
template<class Chain> Chain* link_info(const void* next, VkStructureType type) {
    while (next) {
        const auto* base = static_cast<const VkBaseInStructure*>(next);
        if (base->sType == type) {
            auto* chain = const_cast<Chain*>(static_cast<const Chain*>(next));
            if (chain->function == VK_LAYER_LINK_INFO) return chain;
        }
        next = base->pNext;
    }
    return nullptr;
}
std::string quote(const char* text) {
    std::ostringstream s; s << '"';
    for (const unsigned char c : std::string(text ? text : "")) {
        if (c == '"' || c == '\\') s << '\\' << c;
        else if (c < 32) s << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else s << c;
    }
    s << '"'; return s.str();
}
bool memory_enabled();
bool native_camera_enabled();
bool stack_trace_enabled();
bool openvr_bootstrap_enabled();
class Capture {
public:
    std::mutex mutex;
    std::ofstream events;
    std::filesystem::path directory;
    bool attempted = false;
    uint32_t shader_count = 0;
    uint64_t shader_bytes = 0;
    bool open() {
        if (attempted) return events.is_open();
        attempted = true;
        wchar_t path[32768];
        const auto count = GetEnvironmentVariableW(L"X4VR_CAPTURE_DIR", path, 32768);
        if (!count || count >= 32768) return false;
        const std::filesystem::path root(path);
        if (!root.is_absolute()) return false;
        FILETIME time; GetSystemTimeAsFileTime(&time);
        const uint64_t stamp = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        directory = root / ("process-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp));
        if (!std::filesystem::create_directories(directory)) return false;
        events.open(directory / "events.jsonl", std::ios::out);
        events << "{\"event\":\"capture_started\",\"pid\":" << GetCurrentProcessId()
               << ",\"rendering_modified\":false,\"memory_sampling\":" << (memory_enabled() ? "true" : "false")
               << ",\"native_camera\":" << (native_camera_enabled() ? "true" : "false")
               << ",\"stack_trace\":" << (stack_trace_enabled() ? "true" : "false")
               << ",\"openvr_bootstrap_requested\":" << (openvr_bootstrap_enabled() ? "true" : "false") << "}\n";
        events.flush();
        return events.is_open();
    }
};
Capture capture;
// Diagnostic I/O is never allowed to throw through Vulkan's C ABI.
template<class Writer> void log(Writer writer) noexcept {
    try {
        std::lock_guard lock(capture.mutex);
        if (!capture.open()) return;
        std::ostringstream s; writer(s); capture.events << s.str() << '\n'; capture.events.flush();
    } catch (...) {}
}
std::atomic_uint64_t present_count{}, update_count{}, bind_count{};
std::atomic_uint64_t camera_sample_count{}, world_sample_count{}, camera_epoch{~0ull}, world_epoch{~0ull};
x4vr::observe::CameraSampling camera_sampling;
template<class F> void observe(F function) noexcept { try { function(); } catch (...) {} }
bool memory_enabled() {
    static const bool enabled = [] { wchar_t value[8]{}; return GetEnvironmentVariableW(L"X4VR_CAPTURE_MEMORY", value, 8) == 1 && value[0] == L'1'; }();
    return enabled;
}
bool native_camera_enabled() {
    static const bool enabled = [] { wchar_t value[8]{}; return GetEnvironmentVariableW(L"X4VR_CAPTURE_NATIVE_CAMERA", value, 8) == 1 && value[0] == L'1'; }();
    return enabled;
}
bool stack_trace_enabled() {
    static const bool enabled = [] { wchar_t value[8]{}; return GetEnvironmentVariableW(L"X4VR_CAPTURE_STACK", value, 8) == 1 && value[0] == L'1'; }();
    return enabled;
}
bool openvr_bootstrap_enabled() {
    static const bool enabled = [] { wchar_t value[8]{}; return GetEnvironmentVariableW(L"X4VR_OPENVR_BOOTSTRAP", value, 8) == 1 && value[0] == L'1'; }();
    return enabled;
}
void executable_stack(std::ostream& s) {
    void* frames[40]{};
    const auto count = CaptureStackBackTrace(0, 40, frames, nullptr);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    s << '[';
    bool first = true;
    for (USHORT i = 0; i < count; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        if (address < base || address-base >= nt->OptionalHeader.SizeOfImage) continue;
        if (!first) s << ','; first = false;
        s << address-base;
    }
    s << ']';
}
void sample_uniform(const Device& device, VkCommandBuffer command, VkPipelineLayout layout, uint32_t slot, VkDescriptorSet set) {
    if (!device.memory || (slot != 1 && slot != 3)) return;
    auto& counter = slot == 1 ? camera_sample_count : world_sample_count;
    auto& previous_epoch = slot == 1 ? camera_epoch : world_epoch;
    const auto present = present_count.load();
    uint64_t number{};
    x4vr::observe::Snapshot snapshot;
    if (slot == 1 && native_camera_enabled() && x4vr::observe::supported_executable()) {
        if (!camera_sampling.attempt(present)) return;
        snapshot = device.memory->snapshot(set);
        const auto signature = x4vr::observe::camera_signature(snapshot.bytes.data(), snapshot.size);
        if (!signature) return;
        const auto accepted = camera_sampling.accept(present, *signature);
        if (!accepted) return;
        number = *accepted;
    } else {
        const auto epoch = present/120;
        const auto old_epoch = previous_epoch.exchange(epoch);
        if (counter.load() >= 8 && old_epoch == epoch) return;
        number = counter.fetch_add(1);
        if (number >= 128) return;
        snapshot = device.memory->snapshot(set);
    }
    const auto native = slot == 1 && native_camera_enabled() ? x4vr::observe::capture_native_camera() : x4vr::observe::NativeCamera{};
    log([&](auto& s) {
        s << "{\"event\":\"uniform_snapshot\",\"slot_candidate\":" << slot << ",\"sample\":" << number
          << ",\"present_count\":" << present_count.load() << ",\"thread\":" << GetCurrentThreadId()
          << ",\"command_buffer\":" << handle(command) << ",\"layout\":" << handle(layout) << ",\"set\":" << handle(set)
          << ",\"buffer\":" << handle(snapshot.buffer) << ",\"buffer_offset\":" << snapshot.buffer_offset
          << ",\"memory\":" << handle(snapshot.memory) << ",\"memory_offset\":" << snapshot.memory_offset
          << ",\"range\":" << snapshot.range << ",\"memory_flags\":" << snapshot.memory_flags
          << ",\"status\":" << quote(snapshot.status) << ",\"exe_return_rvas\":";
        // Keep mapped-memory sampling independent of both Windows stack walkers.
        // An empty list with stack_trace=false means disabled, not a failed walk.
        if (stack_trace_enabled()) executable_stack(s);
        else s << "[]";
        if (slot == 1 && native_camera_enabled()) {
            s << ",\"native_camera\":{\"status\":" << quote(native.status)
              << ",\"wrapper_address\":" << native.wrapper << ",\"camera_address\":" << native.camera
              << ",\"temporary_address\":" << native.temporary;
            auto save = [&](const char* label, const auto& bytes, bool valid) {
                if (!valid) return;
                const auto filename = std::string(label) + "-" + std::to_string(number) + ".bin";
                std::ofstream file(capture.directory/filename, std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); file.close();
                if (file) s << ',' << quote(label) << ':' << quote(filename.c_str());
            };
            save("native-wrapper", native.wrapper_bytes, native.wrapper_read);
            save("native-camera", native.camera_bytes, native.camera_read);
            save("native-uniform", native.uniform_bytes, native.uniform_read);
            s << '}';
        }
        if (snapshot.size) {
            const auto filename = "uniform-set" + std::to_string(slot) + "-" + std::to_string(number) + ".bin";
            std::ofstream file(capture.directory/filename, std::ios::binary);
            file.write(reinterpret_cast<const char*>(snapshot.bytes.data()), static_cast<std::streamsize>(snapshot.size)); file.close();
            if (file) s << ",\"file\":" << quote(filename.c_str()) << ",\"bytes\":" << snapshot.size;
        }
        s << '}';
    });
}
void extension_list(std::ostream& s, uint32_t count, const char* const* names) {
    s << '[';
    for (uint32_t i = 0; i < count; ++i) { if (i) s << ','; s << quote(names[i]); }
    s << ']';
}
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkInstance* output) {
    auto* chain = link_info<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gpdpa = chain->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    std::shared_ptr<x4vr::RuntimeBootstrap> runtime;
    std::optional<x4vr::VulkanExtensions> extensions;
    std::vector<const char*> names;
    auto augmented = *ci;
    try {
        if (openvr_bootstrap_enabled() && !x4vr::is_runtime_bootstrap_thread()) {
            runtime = x4vr::acquire_runtime_bootstrap();
            extensions.emplace(ci->enabledExtensionCount, ci->ppEnabledExtensionNames, runtime->instance_extensions());
            names = extensions->names();
            augmented.enabledExtensionCount = static_cast<uint32_t>(names.size());
            augmented.ppEnabledExtensionNames = names.data();
        }
    } catch (const std::exception& error) {
        log([&](auto& s) { s << "{\"event\":\"openvr_bootstrap_failed\",\"stage\":\"instance\",\"error\":" << quote(error.what()) << '}'; });
        *output = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED;
    } catch (...) { *output = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto result = create(&augmented, alloc, output);
    if (result != VK_SUCCESS) return result;
    try {
        std::lock_guard lock(state_mutex);
        instances.emplace(key(*output), Instance{*output, gipa, gpdpa, runtime});
    } catch (...) {
        reinterpret_cast<PFN_vkDestroyInstance>(gipa(*output, "vkDestroyInstance"))(*output, alloc);
        *output = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    log([&](auto& s) {
        s << "{\"event\":\"instance_created\",\"application\":" << quote(ci->pApplicationInfo ? ci->pApplicationInfo->pApplicationName : nullptr)
          << ",\"api_version\":" << (ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0) << ",\"extensions\":";
        extension_list(s, augmented.enabledExtensionCount, augmented.ppEnabledExtensionNames);
        s << ",\"openvr_bootstrap\":" << (runtime ? "true" : "false") << '}';
    });
    return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    auto data = instance_for(instance); if (!data) return;
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(data->gipa(instance, "vkDestroyInstance"));
    { std::lock_guard lock(state_mutex); instances.erase(key(instance)); }
    // This initialization-only runtime has never submitted images or used queues.
    // Release it before the final associated Vulkan instance is destroyed.
    data->runtime.reset();
    destroy(instance, alloc);
    log([](auto& s) { s << "{\"event\":\"instance_destroyed\"}"; });
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkDevice* output) {
    const auto parent = instance_for(physical); if (!parent) return VK_ERROR_INITIALIZATION_FAILED;
    auto* chain = link_info<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(parent->instance, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    std::optional<x4vr::VulkanExtensions> extensions;
    std::vector<const char*> names;
    auto augmented = *ci;
    try {
        if (parent->runtime) {
            // GetOutputDevice returns a loader-facing physical handle; this hook
            // receives the next layer's handle. Compare GPU UUIDs, not wrappers.
            const auto headset_physical = parent->runtime->output_device(parent->instance);
            const auto public_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkGetPhysicalDeviceProperties2"));
            const auto next_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                parent->gipa(parent->instance, "vkGetPhysicalDeviceProperties2"));
            if (!public_properties || !next_properties) throw std::runtime_error("GPU identity query unavailable");
            VkPhysicalDeviceIDProperties headset_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceIDProperties game_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 headset_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            VkPhysicalDeviceProperties2 game_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            headset_properties.pNext = &headset_id; game_properties.pNext = &game_id;
            public_properties(headset_physical, &headset_properties);
            next_properties(physical, &game_properties);
            const uint8_t empty_uuid[VK_UUID_SIZE]{};
            if (!std::memcmp(game_id.deviceUUID, empty_uuid, VK_UUID_SIZE) ||
                std::memcmp(game_id.deviceUUID, headset_id.deviceUUID, VK_UUID_SIZE))
                throw std::runtime_error("X4 selected a GPU different from OpenVR's headset GPU");
            extensions.emplace(ci->enabledExtensionCount, ci->ppEnabledExtensionNames,
                               parent->runtime->device_extensions(headset_physical));
            names = extensions->names();
            augmented.enabledExtensionCount = static_cast<uint32_t>(names.size());
            augmented.ppEnabledExtensionNames = names.data();
        }
    } catch (const std::exception& error) {
        log([&](auto& s) { s << "{\"event\":\"openvr_bootstrap_failed\",\"stage\":\"device\",\"error\":" << quote(error.what()) << '}'; });
        *output = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED;
    } catch (...) { *output = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const auto result = create(physical, &augmented, alloc, output);
    if (result != VK_SUCCESS) return result;
    Device data; data.device = *output; data.gdpa = gdpa;
    data.instance = parent->instance; data.physical = physical; data.gipa = parent->gipa; data.runtime = parent->runtime;
    for (auto* next = static_cast<const VkBaseInStructure*>(ci->pNext); next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(next)->function == VK_LOADER_DATA_CALLBACK)
            data.set_loader_data = reinterpret_cast<const VkLayerDeviceCreateInfo*>(next)->u.pfnSetDeviceLoaderData;
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(parent->gipa(parent->instance, "vkGetPhysicalDeviceMemoryProperties"))(physical, &data.memory_properties);
    if (memory_enabled()) observe([&] { data.memory = std::make_shared<x4vr::observe::MemoryTracker>(); });
#define RESOLVE(name) data.name = reinterpret_cast<PFN_vk##name>(gdpa(*output, "vk" #name));
    DEVICE_FUNCTIONS(RESOLVE)
    PRESENTER_FUNCTIONS(RESOLVE)
#undef RESOLVE
    try { std::lock_guard lock(state_mutex); devices.emplace(key(*output), data); }
    catch (...) { data.DestroyDevice(*output, alloc); *output = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkPhysicalDeviceProperties properties{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(parent->gipa(parent->instance, "vkGetPhysicalDeviceProperties"))(physical, &properties);
    log([&](auto& s) {
        s << "{\"event\":\"device_created\",\"device\":" << handle(*output) << ",\"gpu\":" << quote(properties.deviceName)
          << ",\"extensions\":"; extension_list(s, augmented.enabledExtensionCount, augmented.ppEnabledExtensionNames);
        s << ",\"openvr_bootstrap\":" << (parent->runtime ? "true" : "false") << '}';
    });
    return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    const auto data = device_for(device); if (!data) return;
    { std::lock_guard lock(state_mutex); devices.erase(key(device)); }
    data->DestroyDevice(device, alloc);
    log([&](auto& s) { s << "{\"event\":\"device_destroyed\",\"device\":" << handle(device) << '}'; });
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkShaderModule* output) {
    const auto data = device_for(device); if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = data->CreateShaderModule(device, ci, alloc, output);
    if (result == VK_SUCCESS) log([&](auto& s) {
        s << "{\"event\":\"shader_module\",\"device\":" << handle(device) << ",\"module\":" << handle(*output)
          << ",\"bytes\":" << ci->codeSize;
        if (ci->pCode && ci->codeSize && ci->codeSize <= 4*1024*1024 && capture.shader_count < 4096 &&
            capture.shader_bytes + ci->codeSize <= 128*1024*1024) {
            const auto file = "shader-" + std::to_string(++capture.shader_count) + ".spv";
            std::ofstream binary(capture.directory / file, std::ios::binary);
            binary.write(reinterpret_cast<const char*>(ci->pCode), static_cast<std::streamsize>(ci->codeSize));
            binary.close();
            if (binary) { capture.shader_bytes += ci->codeSize; s << ",\"file\":" << quote(file.c_str()); }
            else s << ",\"capture_error\":\"write_failed\"";
        } else s << ",\"capture_error\":\"limit_or_no_inline_spirv\"";
        s << '}';
    });
    return result;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* output) {
    const auto data = device_for(device); if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = data->CreateDescriptorSetLayout(device, ci, alloc, output);
    if (result == VK_SUCCESS) log([&](auto& s) {
        s << "{\"event\":\"descriptor_layout\",\"layout\":" << handle(*output) << ",\"bindings\":[";
        for (uint32_t i = 0; i < ci->bindingCount; ++i) {
            if (i) s << ','; const auto& b = ci->pBindings[i];
            s << "{\"binding\":" << b.binding << ",\"type\":" << b.descriptorType << ",\"count\":" << b.descriptorCount << ",\"stages\":" << b.stageFlags << '}';
        }
        s << "]}";
    });
    return result;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkPipelineLayout* output) {
    const auto data = device_for(device); if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = data->CreatePipelineLayout(device, ci, alloc, output);
    if (result == VK_SUCCESS) log([&](auto& s) {
        s << "{\"event\":\"pipeline_layout\",\"layout\":" << handle(*output) << ",\"sets\":[";
        for (uint32_t i = 0; i < ci->setLayoutCount; ++i) { if (i) s << ','; s << handle(ci->pSetLayouts[i]); }
        s << "],\"push_constants\":[";
        for (uint32_t i = 0; i < ci->pushConstantRangeCount; ++i) {
            if (i) s << ','; const auto& r = ci->pPushConstantRanges[i];
            s << "{\"offset\":" << r.offset << ",\"size\":" << r.size << ",\"stages\":" << r.stageFlags << '}';
        }
        s << "]}";
    });
    return result;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* alloc, VkPipeline* output) {
    const auto data = device_for(device); if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = data->CreateGraphicsPipelines(device, cache, count, ci, alloc, output);
    for (uint32_t i = 0; i < count; ++i) if (output[i]) log([&](auto& s) {
        s << "{\"event\":\"graphics_pipeline\",\"pipeline\":" << handle(output[i]) << ",\"layout\":" << handle(ci[i].layout)
          << ",\"render_pass\":" << handle(ci[i].renderPass) << ",\"subpass\":" << ci[i].subpass << ",\"shaders\":[";
        for (uint32_t j = 0; j < ci[i].stageCount; ++j) {
            if (j) s << ','; const auto& stage = ci[i].pStages[j];
            s << "{\"stage\":" << stage.stage << ",\"module\":" << handle(stage.module) << ",\"entry\":" << quote(stage.pName) << '}';
        }
        s << "]}";
    });
    return result;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache,
    uint32_t count, const VkComputePipelineCreateInfo* ci, const VkAllocationCallbacks* alloc, VkPipeline* output) {
    const auto data = device_for(device); if (!data) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = data->CreateComputePipelines(device, cache, count, ci, alloc, output);
    for (uint32_t i = 0; i < count; ++i) if (output[i]) log([&](auto& s) {
        s << "{\"event\":\"compute_pipeline\",\"pipeline\":" << handle(output[i]) << ",\"layout\":" << handle(ci[i].layout)
          << ",\"module\":" << handle(ci[i].stage.module) << ",\"entry\":" << quote(ci[i].stage.pName) << '}';
    });
    return result;
}
// Alternate-eye VR presenter. Each presented game frame rendered one eye (the pose
// source advances the shared eye counter once per game frame); copy it into that
// eye's texture, then submit both eye textures to OpenVR with frustum bounds.
struct Presenter {
    std::mutex mutex;
    std::unordered_map<VkQueue, uint32_t> families;
    VkSwapchainKHR swapchain{};
    std::vector<VkImage> images;
    VkExtent2D extent{};
    VkFormat format{};
    // Eye textures are padded so they span each eye's whole frustum at the game's
    // pixels-per-tangent; the game image sits centred, the rest stays black.
    VkExtent2D eye_extent{};
    VkOffset3D offset{};
    float span_x{}, span_y{};
    std::unique_ptr<x4vr::EyeTargets> targets;
    VkCommandPool pool{};
    std::array<VkCommandBuffer, 3> commands{};
    std::array<VkFence, 3> fences{};
    std::array<VkSemaphore, 3> copied{};
    std::array<bool, 2> filled{};
    std::array<x4vr::Matrix, 2> poses{}; // head pose each eye texture was rendered with
    VkPhysicalDevice output_physical{};
    x4vr::RuntimeBootstrap::EyeSetup eyes{};
    uint64_t frame{};
    bool failed{}, ready{};
    // Diagnostic readback of both eye textures, requested by creating dump.txt.
    VkBuffer dump_buffer{};
    VkDeviceMemory dump_memory{};
    bool dump_pending{};
};
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what)+" failed: "+std::to_string(result));
}
std::filesystem::path capture_root() {
    wchar_t root[4096]{};
    return GetEnvironmentVariableW(L"X4VR_CAPTURE_DIR", root, 4096) ? std::filesystem::path(root) : std::filesystem::path();
}
void dump_prepare(const Device& d, Presenter& p) {
    const auto size = VkDeviceSize(p.eye_extent.width)*p.eye_extent.height*4*2;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size; info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(d.CreateBuffer(d.device, &info, nullptr, &p.dump_buffer), "dump buffer");
    VkMemoryRequirements need{};
    d.GetBufferMemoryRequirements(d.device, p.dump_buffer, &need);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = need.size;
    const auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < d.memory_properties.memoryTypeCount; ++i)
        if ((need.memoryTypeBits & (1u << i)) && (d.memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) { allocate.memoryTypeIndex = i; break; }
    check(d.AllocateMemory(d.device, &allocate, nullptr, &p.dump_memory), "dump memory");
    check(d.BindBufferMemory(d.device, p.dump_buffer, p.dump_memory, 0), "dump bind");
}
// Writes eye-0.raw / eye-1.raw (BGRA8, eye_extent) after the slot's fence signalled.
void dump_write(const Device& d, Presenter& p, uint32_t slot) {
    check(d.WaitForFences(d.device, 1, &p.fences[slot], VK_TRUE, UINT64_MAX), "dump wait");
    void* data{};
    const auto bytes = size_t(p.eye_extent.width)*p.eye_extent.height*4;
    check(d.MapMemory(d.device, p.dump_memory, 0, VK_WHOLE_SIZE, 0, &data), "dump map");
    const auto root = capture_root();
    for (int i = 0; i < 2; ++i) {
        std::ofstream out(root/("eye-"+std::to_string(i)+".raw"), std::ios::binary);
        out.write(static_cast<const char*>(data)+i*bytes, std::streamsize(bytes));
    }
    d.UnmapMemory(d.device, p.dump_memory);
    std::ofstream info(root/"eye-dump.txt");
    info << p.eye_extent.width << ' ' << p.eye_extent.height << ' ' << p.format << ' ' << p.offset.x << ' ' << p.offset.y
         << ' ' << p.span_x << ' ' << p.span_y << '\n';
    for (const auto& pose : p.poses) { // submitted (rendered) head pose per eye, row-major 3x4
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) info << pose.m[r][c] << ' ';
        info << '\n';
    }
    OutputDebugStringA("X4VR presenter: eye textures dumped\n");
}
Presenter& presenter() { static auto* value = new Presenter; return *value; }
void presenter_initialize(const Device& d, uint32_t family) {
    auto& p = presenter();
    const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(d.gipa(d.instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto image_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(d.gipa(d.instance, "vkGetPhysicalDeviceImageFormatProperties"));
    p.eyes = d.runtime->eye_setup();
    const float tan_y = x4vr::stereo_settings().game_tan_y;
    const float tan_x = tan_y*float(p.extent.width)/float(p.extent.height);
    p.span_x = tan_x; p.span_y = tan_y;
    for (const auto& t : p.eyes.tangents) {
        p.span_x = std::fmax(p.span_x, std::fmax(std::fabs(t[0]), std::fabs(t[1])));
        p.span_y = std::fmax(p.span_y, std::fmax(std::fabs(t[2]), std::fabs(t[3])));
    }
    p.eye_extent = {uint32_t(std::ceil(p.extent.width*p.span_x/tan_x)), uint32_t(std::ceil(p.extent.height*p.span_y/tan_y))};
    p.offset = {int32_t(p.eye_extent.width-p.extent.width)/2, int32_t(p.eye_extent.height-p.extent.height)/2, 0};
    p.targets = x4vr::EyeTargets::create({d.device, d.physical, d.gdpa, memory, image_properties}, p.eye_extent, p.format);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = family;
    check(d.CreateCommandPool(d.device, &pool, nullptr, &p.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = p.pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 3;
    check(d.AllocateCommandBuffers(d.device, &allocate, p.commands.data()), "vkAllocateCommandBuffers");
    for (int i = 0; i < 3; ++i) {
        // Layer-created dispatchable objects need the loader's dispatch pointer.
        if (d.set_loader_data) check(d.set_loader_data(d.device, p.commands[i]), "vkSetDeviceLoaderData");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(d.CreateFence(d.device, &fence, nullptr, &p.fences[i]), "vkCreateFence");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(d.CreateSemaphore(d.device, &semaphore, nullptr, &p.copied[i]), "vkCreateSemaphore");
    }
    p.output_physical = d.runtime->output_device(d.instance);
    log([&](auto& s) {
        s << "{\"event\":\"vr_presenter_ready\",\"width\":" << p.extent.width << ",\"height\":" << p.extent.height
          << ",\"format\":" << p.format << ",\"tangents\":[";
        for (int i = 0; i < 2; ++i) for (int k = 0; k < 4; ++k) s << (i||k ? "," : "") << p.eyes.tangents[i][k];
        s << "],\"eye_x\":[" << p.eyes.head_from_eye[0].m[0][3] << ',' << p.eyes.head_from_eye[1].m[0][3]
          << "],\"eye_extent\":[" << p.eye_extent.width << ',' << p.eye_extent.height << "],\"span\":[" << p.span_x << ',' << p.span_y << "]}";
    });
    OutputDebugStringA("X4VR presenter: eye targets ready; alternate-eye OpenVR submission active\n");
    p.ready = true;
}
void barrier(const Device& d, VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}
// Returns the semaphore the real present must wait on, or null to present unchanged.
VkSemaphore presenter_copy(const Device& d, VkQueue queue, const VkPresentInfoKHR* info) {
    auto& p = presenter();
    if (p.failed || !d.runtime || info->swapchainCount != 1 || info->pSwapchains[0] != p.swapchain) return VK_NULL_HANDLE;
    const auto family = p.families.find(queue);
    if (family == p.families.end() || info->pImageIndices[0] >= p.images.size()) return VK_NULL_HANDLE;
    if (!p.ready) presenter_initialize(d, family->second);
    const auto settings = x4vr::stereo_settings();
    const auto number = x4vr::next_present();
    const uint32_t eye = settings.stereo ? static_cast<uint32_t>(number & 1) : 0;
    x4vr::Matrix rendered;
    if (!x4vr::render_pose_for(number, rendered)) return VK_NULL_HANDLE; // unknown pose: skip frame

    const auto slot = p.frame++ % 3;
    check(d.WaitForFences(d.device, 1, &p.fences[slot], VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(d.ResetFences(d.device, 1, &p.fences[slot]), "vkResetFences");
    const auto command = p.commands[slot];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(d.BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    const auto source = p.images[info->pImageIndices[0]];
    barrier(d, command, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    for (uint32_t target = 0; target < 2; ++target) {
        if (settings.stereo && target != eye) continue; // mono mode fills both eyes
        const auto image = p.targets->eyes()[target].color.image;
        barrier(d, command, image, p.filled[target] ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (!p.filled[target]) {
            const VkClearColorValue black{};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            d.CmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            barrier(d, command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstOffset = p.offset;
        copy.extent = {p.extent.width, p.extent.height, 1};
        d.CmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(d, command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        p.filled[target] = true;
        p.poses[target] = rendered;
    }
    barrier(d, command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0);
    p.dump_pending = false;
    if (p.filled[0] && p.filled[1] && std::filesystem::remove(capture_root()/"dump.txt")) {
        if (!p.dump_buffer) dump_prepare(d, p);
        for (uint32_t i = 0; i < 2; ++i) {
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(i)*p.eye_extent.width*p.eye_extent.height*4;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {p.eye_extent.width, p.eye_extent.height, 1};
            d.CmdCopyImageToBuffer(command, p.targets->eyes()[i].color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.dump_buffer, 1, &region);
        }
        p.dump_pending = true;
    }
    check(d.EndCommandBuffer(command), "vkEndCommandBuffer");
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info->waitSemaphoreCount; submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &p.copied[slot];
    check(d.QueueSubmit(queue, 1, &submit, p.fences[slot]), "vkQueueSubmit");
    if (p.dump_pending) dump_write(d, p, static_cast<uint32_t>(slot));
    return p.copied[slot];
}
void presenter_submit(const Device& d, VkQueue queue) {
    auto& p = presenter();
    if (!p.filled[0] || !p.filled[1]) return;
    // The padded texture spans [-span, span] tangents symmetrically; crop each
    // eye's asymmetric frustum out of it. Tangent convention: OpenVR top is negative.
    std::array<vr::VRTextureBounds_t, 2> bounds{};
    std::array<vr::VRVulkanTextureData_t, 2> textures{};
    for (int i = 0; i < 2; ++i) {
        const auto& t = p.eyes.tangents[i];
        // GetProjectionRaw's top/bottom are flipped relative to texture v (Valve's plugin:
        // vMin = 0.5-0.5*bottom/tanY, vMax = 0.5-0.5*top/tanY).
        if (x4vr::stereo_settings().valve_bounds)
            bounds[i] = {0.5f+0.5f*t[0]/p.span_x, 0.5f-0.5f*t[3]/p.span_y, 0.5f+0.5f*t[1]/p.span_x, 0.5f-0.5f*t[2]/p.span_y};
        else
            bounds[i] = {0.5f+0.5f*t[0]/p.span_x, 0.5f+0.5f*t[2]/p.span_y, 0.5f+0.5f*t[1]/p.span_x, 0.5f+0.5f*t[3]/p.span_y};
        auto& v = textures[i];
        v.m_nImage = reinterpret_cast<uint64_t>(p.targets->eyes()[i].color.image);
        v.m_pDevice = d.device; v.m_pPhysicalDevice = p.output_physical; v.m_pInstance = d.instance;
        v.m_pQueue = queue; v.m_nQueueFamilyIndex = p.families[queue];
        v.m_nWidth = p.eye_extent.width; v.m_nHeight = p.eye_extent.height; v.m_nFormat = p.format; v.m_nSampleCount = 1;
    }
    if (!x4vr::stereo_settings().pace) return; // calibration: no compositor pacing/submission
    const auto error = d.runtime->submit_stereo(textures, bounds, p.poses);
    static std::atomic_uint reported{};
    if (!error.empty() && reported++ < 8) OutputDebugStringA(("X4VR presenter: "+error+"\n").c_str());
    static std::atomic_bool first{};
    if (error.empty() && !first.exchange(true)) OutputDebugStringA("X4VR presenter: first stereo pair submitted to OpenVR\n");
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* alloc, VkSwapchainKHR* output) {
    const auto data = device_for(device); if (!data || !data->CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = data->CreateSwapchainKHR(device, ci, alloc, output);
    log([&](auto& s) {
        s << "{\"event\":\"swapchain\",\"result\":" << result << ",\"width\":" << ci->imageExtent.width
          << ",\"height\":" << ci->imageExtent.height << ",\"format\":" << ci->imageFormat << ",\"usage\":" << ci->imageUsage << '}';
    });
    if (result == VK_SUCCESS && data->runtime && (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) observe([&] {
        auto& p = presenter();
        std::lock_guard lock(p.mutex);
        if (p.ready && (ci->imageExtent.width != p.extent.width || ci->imageExtent.height != p.extent.height || ci->imageFormat != p.format)) {
            p.failed = true; // ponytail: no eye-target rebuild on resize; restart X4 after changing resolution
            OutputDebugStringA("X4VR presenter: swapchain size changed; VR submission stopped\n");
            return;
        }
        uint32_t count{};
        data->GetSwapchainImagesKHR(device, *output, &count, nullptr);
        p.images.resize(count);
        data->GetSwapchainImagesKHR(device, *output, &count, p.images.data());
        p.swapchain = *output; p.extent = ci->imageExtent; p.format = ci->imageFormat;
    });
    return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    const auto data = device_for(device); if (!data || !data->GetDeviceQueue) return;
    data->GetDeviceQueue(device, family, index, queue);
    if (*queue) { auto& p = presenter(); std::lock_guard lock(p.mutex); p.families[*queue] = family; }
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    const auto data = device_for(device); if (!data || !data->GetDeviceQueue2) return;
    data->GetDeviceQueue2(device, info, queue);
    if (*queue) { auto& p = presenter(); std::lock_guard lock(p.mutex); p.families[*queue] = info->queueFamilyIndex; }
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    const auto data = device_for(queue); if (!data || !data->QueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;
    auto& p = presenter();
    std::unique_lock lock(p.mutex);
    VkSemaphore copied{};
    try { copied = presenter_copy(*data, queue, info); }
    catch (const std::exception& error) {
        p.failed = true;
        OutputDebugStringA(("X4VR presenter disabled: "+std::string(error.what())+"\n").c_str());
    }
    auto forwarded = *info;
    if (copied) { forwarded.waitSemaphoreCount = 1; forwarded.pWaitSemaphores = &copied; }
    const auto result = data->QueuePresentKHR(queue, &forwarded);
    if (copied) try { presenter_submit(*data, queue); } catch (...) {}
    lock.unlock();
    const auto frame = ++present_count;
    if (frame <= 5 || frame % 300 == 0) log([&](auto& s) {
        s << "{\"event\":\"present\",\"number\":" << frame << ",\"result\":" << result << ",\"tick\":" << GetTickCount64() << '}';
    });
    return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t count,
    const VkWriteDescriptorSet* writes, uint32_t copy_count, const VkCopyDescriptorSet* copies) {
    const auto data = device_for(device); if (!data) return;
    data->UpdateDescriptorSets(device, count, writes, copy_count, copies);
    if (data->memory) observe([&] { data->memory->update(count, writes, copy_count, copies); });
    if (update_count.fetch_add(1) >= 2048) return;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& w = writes[i];
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && w.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        log([&](auto& s) {
            s << "{\"event\":\"uniform_descriptor_write\",\"set\":" << handle(w.dstSet) << ",\"binding\":" << w.dstBinding
              << ",\"array_element\":" << w.dstArrayElement << ",\"type\":" << w.descriptorType << ",\"buffers\":[";
            for (uint32_t j = 0; j < w.descriptorCount && j < 64; ++j) {
                if (j) s << ','; const auto& b = w.pBufferInfo[j];
                s << "{\"buffer\":" << handle(b.buffer) << ",\"offset\":" << b.offset << ",\"range\":" << b.range << '}';
            }
            s << "]}";
        });
    }
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t dynamic_count, const uint32_t* dynamic_offsets) {
    const auto data = device_for(cmd); if (!data) return;
    data->CmdBindDescriptorSets(cmd, point, layout, first, count, sets, dynamic_count, dynamic_offsets);
    if (data->memory) observe([&] {
        for (uint32_t i = 0; i < count; ++i) sample_uniform(*data, cmd, layout, first+i, sets[i]);
    });
    if (bind_count.fetch_add(1) >= 2048) return;
    log([&](auto& s) {
        s << "{\"event\":\"bind_descriptor_sets\",\"command_buffer\":" << handle(cmd) << ",\"layout\":" << handle(layout)
          << ",\"bind_point\":" << point << ",\"first_set\":" << first << ",\"sets\":[";
        for (uint32_t i = 0; i < count; ++i) { if (i) s << ','; s << handle(sets[i]); }
        s << "],\"dynamic_offsets\":[";
        for (uint32_t i = 0; i < dynamic_count; ++i) { if (i) s << ','; s << dynamic_offsets[i]; }
        s << "]}";
    });
}
// Tracking never owns these allocations. Untrack before destructive downstream calls
// so no concurrent snapshot can retain a pointer after Vulkan unmaps/frees it.
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkBuffer* output) {
    const auto d = device_for(device); if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = d->CreateBuffer(device, ci, alloc, output);
    if (result == VK_SUCCESS && d->memory) observe([&] { d->memory->create_buffer(*output, ci->size); });
    return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* alloc) {
    const auto d = device_for(device); if (!d) return;
    if (d->memory) observe([&] { d->memory->destroy_buffer(buffer); });
    d->DestroyBuffer(device, buffer, alloc);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* ci,
    const VkAllocationCallbacks* alloc, VkDeviceMemory* output) {
    const auto d = device_for(device); if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = d->AllocateMemory(device, ci, alloc, output);
    if (result == VK_SUCCESS && d->memory) observe([&] {
        d->memory->allocate(*output, ci->allocationSize, d->memory_properties.memoryTypes[ci->memoryTypeIndex].propertyFlags);
    });
    return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
    const auto d = device_for(device); if (!d) return;
    if (d->memory) observe([&] { d->memory->free(memory); });
    d->FreeMemory(device, memory, alloc);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    const auto d = device_for(device); if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = d->BindBufferMemory(device, buffer, memory, offset);
    if (result == VK_SUCCESS && d->memory) observe([&] { d->memory->bind(buffer, memory, offset); });
    return result;
}
#define BIND_MEMORY2(name) \
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk##name(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* info) { \
    const auto d = device_for(device); if (!d || !d->name) return VK_ERROR_INITIALIZATION_FAILED; \
    const auto result = d->name(device, count, info); \
    if (result == VK_SUCCESS && d->memory) observe([&] { \
        for (uint32_t i = 0; i < count; ++i) d->memory->bind(info[i].buffer, info[i].memory, info[i].memoryOffset); \
    }); \
    return result; \
}
BIND_MEMORY2(BindBufferMemory2)
BIND_MEMORY2(BindBufferMemory2KHR)
#undef BIND_MEMORY2
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void** output) {
    const auto d = device_for(device); if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = d->MapMemory(device, memory, offset, size, flags, output);
    if (result == VK_SUCCESS && d->memory) observe([&] { d->memory->map(memory, offset, size, *output); });
    return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    const auto d = device_for(device); if (!d) return;
    if (d->memory) observe([&] { d->memory->unmap(memory); });
    d->UnmapMemory(device, memory);
}
#define MAP_MEMORY2(name) \
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk##name(VkDevice device, const VkMemoryMapInfo* info, void** output) { \
    const auto d = device_for(device); if (!d || !d->name) return VK_ERROR_INITIALIZATION_FAILED; \
    const auto result = d->name(device, info, output); \
    if (result == VK_SUCCESS && d->memory) observe([&] { d->memory->map(info->memory, info->offset, info->size, *output); }); \
    return result; \
}
MAP_MEMORY2(MapMemory2)
MAP_MEMORY2(MapMemory2KHR)
#undef MAP_MEMORY2
#define UNMAP_MEMORY2(name) \
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk##name(VkDevice device, const VkMemoryUnmapInfo* info) { \
    const auto d = device_for(device); if (!d || !d->name) return VK_ERROR_INITIALIZATION_FAILED; \
    if (d->memory) observe([&] { d->memory->unmap(info->memory); }); \
    return d->name(device, info); \
}
UNMAP_MEMORY2(UnmapMemory2)
UNMAP_MEMORY2(UnmapMemory2KHR)
#undef UNMAP_MEMORY2
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* output) {
    const auto d = device_for(device); if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = d->AllocateDescriptorSets(device, info, output);
    if (result == VK_SUCCESS && d->memory) observe([&] { d->memory->allocate_sets(info->descriptorPool, info->descriptorSetCount, output); });
    return result;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet* sets) {
    const auto d = device_for(device); if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->memory) observe([&] { d->memory->free_sets(count, sets); });
    return d->FreeDescriptorSets(device, pool, count, sets);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
    const auto d = device_for(device); if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->memory) observe([&] { d->memory->reset_pool(pool); });
    return d->ResetDescriptorPool(device, pool, flags);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* alloc) {
    const auto d = device_for(device); if (!d) return;
    if (d->memory) observe([&] { d->memory->reset_pool(pool); });
    d->DestroyDescriptorPool(device, pool, alloc);
}
namespace {
PFN_vkVoidFunction device_intercept(const char* name) {
#define MATCH(fn) if (!std::strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(vk##fn);
    DEVICE_FUNCTIONS(MATCH)
#undef MATCH
    return nullptr;
}
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
#define MATCH(fn) if (!std::strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(vk##fn);
    MATCH(GetInstanceProcAddr) MATCH(CreateInstance)
    if (!instance) return nullptr;
    MATCH(GetDeviceProcAddr) MATCH(DestroyInstance) MATCH(CreateDevice)
#undef MATCH
    const auto data = instance_for(instance); if (!data) return nullptr;
    const auto next = data->gipa(instance, name);
    if (next) if (const auto intercepted = device_intercept(name)) return intercepted;
    return next;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name || !device) return nullptr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    const auto data = device_for(device); if (!data) return nullptr;
    const auto next = data->gdpa(device, name);
    if (next) if (const auto intercepted = device_intercept(name)) return intercepted;
    return next;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    if (!name || !instance) return nullptr;
    const auto data = instance_for(instance);
    return data && data->gpdpa ? data->gpdpa(instance, name) : nullptr;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL x4vrNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (!info || info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    info->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    info->pfnGetPhysicalDeviceProcAddr = vkGetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}
