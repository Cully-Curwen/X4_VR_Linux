#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace x4vr::observe {
struct Snapshot {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize buffer_offset{}, memory_offset{}, range{};
    VkMemoryPropertyFlags memory_flags{};
    const char* status = "descriptor_unknown";
    std::array<std::byte, 2048> bytes{};
    size_t size{};
};
// Per-device observational state. All reads are bounded to a live mapped range.
// No Vulkan map, invalidate, write or GPU synchronization is performed here.
class MemoryTracker {
    struct Allocation {
        VkDeviceSize size{}, map_offset{}, map_size{};
        VkMemoryPropertyFlags flags{};
        const std::byte* mapped{};
    };
    struct Buffer { VkDeviceSize size{}, offset{}; VkDeviceMemory memory{}; };
    struct Descriptor { VkDescriptorBufferInfo buffer{}; VkDescriptorType type{}; };
    std::mutex mutex_;
    std::unordered_map<VkDeviceMemory, Allocation> allocations_;
    std::unordered_map<VkBuffer, Buffer> buffers_;
    std::unordered_map<VkDescriptorSet, Descriptor> descriptors_;
    std::unordered_map<VkDescriptorSet, VkDescriptorPool> pools_;
public:
    void allocate(VkDeviceMemory memory, VkDeviceSize size, VkMemoryPropertyFlags flags) {
        std::lock_guard lock(mutex_);
        allocations_[memory] = {size, 0, 0, flags, nullptr};
    }
    void free(VkDeviceMemory memory) { std::lock_guard lock(mutex_); allocations_.erase(memory); }
    void create_buffer(VkBuffer buffer, VkDeviceSize size) {
        std::lock_guard lock(mutex_); buffers_[buffer] = {size, 0, nullptr};
    }
    void destroy_buffer(VkBuffer buffer) { std::lock_guard lock(mutex_); buffers_.erase(buffer); }
    void bind(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
        std::lock_guard lock(mutex_);
        if (auto b = buffers_.find(buffer); b != buffers_.end()) { b->second.memory = memory; b->second.offset = offset; }
    }
    void map(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, const void* address) {
        std::lock_guard lock(mutex_);
        auto i = allocations_.find(memory); if (i == allocations_.end()) return;
        auto& a = i->second;
        if (offset > a.size) return;
        a.map_offset = offset; a.map_size = size == VK_WHOLE_SIZE ? a.size-offset : size;
        if (a.map_size > a.size-offset) { a.map_size = 0; return; }
        a.mapped = static_cast<const std::byte*>(address);
    }
    void unmap(VkDeviceMemory memory) {
        std::lock_guard lock(mutex_);
        if (auto i = allocations_.find(memory); i != allocations_.end()) i->second.mapped = nullptr;
    }
    void allocate_sets(VkDescriptorPool pool, uint32_t count, const VkDescriptorSet* sets) {
        std::lock_guard lock(mutex_);
        for (uint32_t i = 0; i < count; ++i) { descriptors_.erase(sets[i]); pools_[sets[i]] = pool; }
    }
    void free_sets(uint32_t count, const VkDescriptorSet* sets) {
        std::lock_guard lock(mutex_);
        for (uint32_t i = 0; i < count; ++i) { descriptors_.erase(sets[i]); pools_.erase(sets[i]); }
    }
    void reset_pool(VkDescriptorPool pool) {
        std::lock_guard lock(mutex_);
        for (auto i = pools_.begin(); i != pools_.end();) {
            if (i->second == pool) { descriptors_.erase(i->first); i = pools_.erase(i); }
            else ++i;
        }
    }
    void update(uint32_t count, const VkWriteDescriptorSet* writes, uint32_t copy_count, const VkCopyDescriptorSet* copies) {
        std::lock_guard lock(mutex_);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& w = writes[i];
            if (w.dstBinding != 0 || w.dstArrayElement != 0) continue;
            if (w.descriptorCount == 1 && w.pBufferInfo &&
                (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))
                descriptors_[w.dstSet] = {*w.pBufferInfo, w.descriptorType};
            else descriptors_.erase(w.dstSet);
        }
        for (uint32_t i = 0; i < copy_count; ++i) {
            const auto& c = copies[i];
            if (c.dstBinding != 0 || c.dstArrayElement != 0) continue;
            const auto source = descriptors_.find(c.srcSet);
            if (c.descriptorCount == 1 && c.srcBinding == 0 && c.srcArrayElement == 0 && source != descriptors_.end())
                descriptors_[c.dstSet] = source->second;
            else descriptors_.erase(c.dstSet);
        }
    }
    Snapshot snapshot(VkDescriptorSet set) {
        std::lock_guard lock(mutex_);
        Snapshot s;
        const auto d = descriptors_.find(set); if (d == descriptors_.end()) return s;
        if (d->second.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) { s.status = "dynamic_descriptor_not_sampled"; return s; }
        const auto& descriptor = d->second.buffer;
        s.buffer = descriptor.buffer; s.buffer_offset = descriptor.offset; s.range = descriptor.range;
        const auto b = buffers_.find(s.buffer);
        if (b == buffers_.end()) { s.status = "buffer_unknown"; return s; }
        const auto& buffer = b->second; s.memory = buffer.memory;
        const auto a = allocations_.find(buffer.memory);
        if (a == allocations_.end()) { s.status = "memory_unknown"; return s; }
        const auto& allocation = a->second; s.memory_flags = allocation.flags;
        if (!allocation.mapped) { s.status = "not_mapped"; return s; }
        if (descriptor.offset > buffer.size || buffer.offset > allocation.size ||
            descriptor.offset > allocation.size-buffer.offset) { s.status = "invalid_buffer_range"; return s; }
        s.memory_offset = buffer.offset + descriptor.offset;
        if (s.memory_offset < allocation.map_offset || s.memory_offset-allocation.map_offset > allocation.map_size) {
            s.status = "outside_mapped_range"; return s;
        }
        const auto delta = s.memory_offset-allocation.map_offset;
        const auto remaining = std::min({buffer.size-descriptor.offset, allocation.size-s.memory_offset,
                                         allocation.map_size-delta, descriptor.range, VkDeviceSize(s.bytes.size())});
        if (!remaining) { s.status = "empty_range"; return s; }
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), allocation.mapped+delta, s.bytes.data(), static_cast<SIZE_T>(remaining), &read)) {
            s.status = "cpu_read_failed"; return s;
        }
        s.size = read; s.status = "mapped_cpu_snapshot";
        return s;
    }
};
}
