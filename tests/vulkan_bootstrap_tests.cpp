#include <x4vr/vulkan_extensions.hpp>
#include <iostream>

void require(bool value) { if (!value) throw std::runtime_error("Bootstrap contract failed"); }
int main() {
    try {
        const char* application[]{"VK_KHR_surface", "VK_KHR_win32_surface"};
        x4vr::VulkanExtensions merged(2, application, {"VK_KHR_surface", "VK_KHR_external_memory_capabilities",
                                                     "VK_KHR_external_memory_capabilities"});
        const auto names = merged.names();
        require(names.size() == 3);
        require(std::string(names[0]) == application[0] && std::string(names[1]) == application[1]);
        require(std::string(names[2]) == "VK_KHR_external_memory_capabilities");
        require(x4vr::VulkanExtensions(0, nullptr, {}).names().empty());
        bool rejected = false;
        try { x4vr::VulkanExtensions invalid(1, nullptr, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected);
        rejected = false;
        try { x4vr::VulkanExtensions invalid(0, nullptr, {""}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected);
        std::cout << "Extension merge and invalid-list rejection passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
