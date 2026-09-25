#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
template<class Function, class Resolver, class Object>
Function resolve(Resolver resolver, Object object, const char* name) {
    auto function = reinterpret_cast<Function>(resolver(object, name));
    if (!function) throw std::runtime_error(std::string("Missing Vulkan function: ") + name);
    return function;
}
}
int main() {
    // This is a standalone process, never attached to the game. If an error occurs,
    // process termination reclaims its partially constructed Vulkan resources.
    try {
        const auto library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) throw std::runtime_error("Vulkan loader unavailable");
        const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
        if (!gipa) throw std::runtime_error("Loader has no vkGetInstanceProcAddr");
        const char* layer = "VK_LAYER_X4VR_observe";
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "x4vr-vulkan-smoke"; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app; ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer;
        VkInstance instance{};
        auto create_instance = resolve<PFN_vkCreateInstance>(gipa, VkInstance{}, "vkCreateInstance");
        check(create_instance(&ici, nullptr, &instance), "create instance with observation layer");
#define INSTANCE_FN(name) auto name = resolve<PFN_vk##name>(gipa, instance, "vk" #name)
        INSTANCE_FN(EnumeratePhysicalDevices); INSTANCE_FN(GetPhysicalDeviceProperties);
        INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties); INSTANCE_FN(GetPhysicalDeviceMemoryProperties);
        INSTANCE_FN(CreateDevice); INSTANCE_FN(GetDeviceProcAddr); INSTANCE_FN(DestroyInstance);
        uint32_t physical_count = 0;
        check(EnumeratePhysicalDevices(instance, &physical_count, nullptr), "physical device count");
        if (!physical_count) throw std::runtime_error("No Vulkan device");
        std::vector<VkPhysicalDevice> physicals(physical_count);
        check(EnumeratePhysicalDevices(instance, &physical_count, physicals.data()), "physical devices");
        const auto physical = physicals[0];
        VkPhysicalDeviceProperties properties{}; GetPhysicalDeviceProperties(physical, &properties);
        std::cout << "gpu=" << properties.deviceName << '\n';
        uint32_t queue_count = 0; GetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        GetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues.data());
        uint32_t family = 0;
        while (family < queue_count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        if (family == queue_count) throw std::runtime_error("No compute queue");
        const float priority = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        VkDevice device{}; check(CreateDevice(physical, &dci, nullptr, &device), "create device");
#define DEVICE_FN(name) auto name = resolve<PFN_vk##name>(GetDeviceProcAddr, device, "vk" #name)
        DEVICE_FN(CreateShaderModule); DEVICE_FN(DestroyShaderModule);
        DEVICE_FN(CreateDescriptorSetLayout); DEVICE_FN(DestroyDescriptorSetLayout);
        DEVICE_FN(CreatePipelineLayout); DEVICE_FN(DestroyPipelineLayout);
        DEVICE_FN(CreateComputePipelines); DEVICE_FN(DestroyPipeline);
        DEVICE_FN(CreateBuffer); DEVICE_FN(GetBufferMemoryRequirements); DEVICE_FN(AllocateMemory);
        DEVICE_FN(BindBufferMemory); DEVICE_FN(FreeMemory); DEVICE_FN(DestroyBuffer);
        DEVICE_FN(MapMemory); DEVICE_FN(UnmapMemory);
        DEVICE_FN(CreateDescriptorPool); DEVICE_FN(AllocateDescriptorSets); DEVICE_FN(UpdateDescriptorSets); DEVICE_FN(DestroyDescriptorPool);
        DEVICE_FN(CreateCommandPool); DEVICE_FN(AllocateCommandBuffers); DEVICE_FN(BeginCommandBuffer); DEVICE_FN(EndCommandBuffer);
        DEVICE_FN(CmdBindDescriptorSets); DEVICE_FN(CmdBindPipeline); DEVICE_FN(CmdDispatch); DEVICE_FN(DestroyCommandPool);
        DEVICE_FN(GetDeviceQueue); DEVICE_FN(QueueSubmit); DEVICE_FN(QueueWaitIdle); DEVICE_FN(DestroyDevice);
        // Valid SPIR-V 1.0: a compute entry point with local_size=(1,1,1), empty body.
        const uint32_t code[]{0x07230203, 0x00010000, 0, 5, 0,
            0x00020011,1, 0x0003000e,0,1, 0x0005000f,5,3,0x6e69616d,0,
            0x00060010,3,17,1,1,1, 0x00020013,1, 0x00030021,2,1,
            0x00050036,1,3,0,2, 0x000200f8,4, 0x000100fd, 0x00010038};
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sci.codeSize = sizeof(code); sci.pCode = code;
        VkShaderModule shader{}; check(CreateShaderModule(device, &sci, nullptr, &shader), "create shader");
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; slci.bindingCount = 1; slci.pBindings = &binding;
        VkDescriptorSetLayout set_layout{}; check(CreateDescriptorSetLayout(device, &slci, nullptr, &set_layout), "create descriptor layout");
        const VkDescriptorSetLayout layouts[]{set_layout, set_layout};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount = 2; plci.pSetLayouts = layouts;
        VkPipelineLayout pipeline_layout{}; check(CreatePipelineLayout(device, &plci, nullptr, &pipeline_layout), "create pipeline layout");
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.layout = pipeline_layout; cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = shader; cpci.stage.pName = "main";
        VkPipeline pipeline{}; check(CreateComputePipelines(device, nullptr, 1, &cpci, nullptr, &pipeline), "create compute pipeline");
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = 4096; bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VkBuffer buffer{}; check(CreateBuffer(device, &bci, nullptr, &buffer), "create uniform buffer");
        VkMemoryRequirements requirements{}; GetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{}; GetPhysicalDeviceMemoryProperties(physical, &memory);
        uint32_t memory_type = 0;
        const auto required_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (memory_type < memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << memory_type)) ||
            (memory.memoryTypes[memory_type].propertyFlags & required_flags) != required_flags)) ++memory_type;
        if (memory_type == memory.memoryTypeCount) throw std::runtime_error("No compatible memory type");
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = requirements.size; mai.memoryTypeIndex = memory_type;
        VkDeviceMemory allocation{}; check(AllocateMemory(device, &mai, nullptr, &allocation), "allocate buffer memory");
        check(BindBufferMemory(device, buffer, allocation, 0), "bind buffer memory");
        void* mapped{}; check(MapMemory(device, allocation, 0, VK_WHOLE_SIZE, 0, &mapped), "map uniform memory");
        const float fixture[]{1,0,0,0, 0,1,0,0, 0,0,1,0, 4,5,6,1};
        std::memcpy(mapped, fixture, sizeof(fixture));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &size;
        VkDescriptorPool pool{}; check(CreateDescriptorPool(device, &dpci, nullptr, &pool), "create descriptor pool");
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout;
        VkDescriptorSet set{}; check(AllocateDescriptorSets(device, &dsai, &set), "allocate descriptor set");
        VkDescriptorBufferInfo buffer_info{buffer, 0, 64};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &buffer_info;
        UpdateDescriptorSets(device, 1, &write, 0, nullptr);
        VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; command_pool_info.queueFamilyIndex = family;
        VkCommandPool command_pool{}; check(CreateCommandPool(device, &command_pool_info, nullptr, &command_pool), "create command pool");
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = command_pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer command{}; check(AllocateCommandBuffers(device, &cbai, &command), "allocate command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(BeginCommandBuffer(command, &begin), "begin command buffer");
        CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 1, 1, &set, 0, nullptr);
        CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); CmdDispatch(command, 1, 1, 1);
        check(EndCommandBuffer(command), "end command buffer");
        VkQueue queue{}; GetDeviceQueue(device, family, 0, &queue);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        check(QueueSubmit(queue, 1, &submit, nullptr), "submit GPU compute work");
        check(QueueWaitIdle(queue), "wait for GPU completion");
        DestroyCommandPool(device, command_pool, nullptr); DestroyDescriptorPool(device, pool, nullptr);
        UnmapMemory(device, allocation);
        DestroyBuffer(device, buffer, nullptr); FreeMemory(device, allocation, nullptr);
        DestroyPipeline(device, pipeline, nullptr); DestroyPipelineLayout(device, pipeline_layout, nullptr);
        DestroyDescriptorSetLayout(device, set_layout, nullptr); DestroyShaderModule(device, shader, nullptr);
        DestroyDevice(device, nullptr); DestroyInstance(instance, nullptr); FreeLibrary(library);
        std::cout << "Observation layer: instance/device/shader/descriptors/compute dispatch/cleanup succeeded.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
