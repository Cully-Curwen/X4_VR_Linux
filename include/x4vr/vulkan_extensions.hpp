#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace x4vr {
// Own names across a downstream creation call. Preserve the application's list
// and append every runtime requirement, without mutating its create info/pNext.
class VulkanExtensions {
    std::vector<std::string> strings_;
public:
    VulkanExtensions(uint32_t count, const char* const* application,
                     const std::vector<std::string>& required) {
        if (count && !application) throw std::invalid_argument("Missing Vulkan extension list");
        for (uint32_t i = 0; i < count; ++i) {
            if (!application[i]) throw std::invalid_argument("Null Vulkan extension name");
            strings_.emplace_back(application[i]);
        }
        for (const auto& name : required) {
            if (name.empty()) throw std::invalid_argument("Empty runtime extension name");
            if (std::find(strings_.begin(), strings_.end(), name) == strings_.end()) strings_.push_back(name);
        }
    }
    std::vector<const char*> names() const {
        std::vector<const char*> result;
        for (const auto& name : strings_) result.push_back(name.c_str());
        return result;
    }
};
}
