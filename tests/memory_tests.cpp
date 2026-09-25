#include "observe_memory.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace x4vr::observe;
    unsigned checks = 0;
    auto require = [&](bool condition) { ++checks; if (!condition) throw std::runtime_error("memory tracking check " + std::to_string(checks)); };
    try {
        MemoryTracker t;
        auto m = reinterpret_cast<VkDeviceMemory>(1);
        auto b = reinterpret_cast<VkBuffer>(2);
        auto s = reinterpret_cast<VkDescriptorSet>(3);
        auto p = reinterpret_cast<VkDescriptorPool>(4);
        std::array<std::byte, 512> storage{};
        storage[96] = std::byte{42};
        t.allocate(m, 1024, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        t.create_buffer(b, 256); t.bind(b, m, 128);
        t.allocate_sets(p, 1, &s);
        VkDescriptorBufferInfo info{b, 32, 64};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = s; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &info;
        t.update(1, &write, 0, nullptr);
        require(t.snapshot(s).size == 0);
        t.map(m, 64, storage.size(), storage.data());
        auto snap = t.snapshot(s);
        require(snap.size == 64 && snap.bytes[0] == std::byte{42});
        require(snap.memory_offset == 160);
        t.unmap(m); require(t.snapshot(s).size == 0);
        t.map(m, 200, storage.size(), storage.data());
        require(t.snapshot(s).size == 0);
        t.map(m, 64, 100, storage.data());
        require(t.snapshot(s).size == 4);
        info.offset = 257; t.update(1, &write, 0, nullptr);
        require(t.snapshot(s).size == 0);
        info.offset = 32; info.range = VK_WHOLE_SIZE; t.update(1, &write, 0, nullptr);
        t.map(m, 64, storage.size(), storage.data());
        require(t.snapshot(s).size == 224);
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        t.update(1, &write, 0, nullptr); require(t.snapshot(s).size == 0);
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        t.update(1, &write, 0, nullptr);
        t.reset_pool(p); require(t.snapshot(s).size == 0);
        t.update(1, &write, 0, nullptr);
        t.allocate_sets(p, 1, &s); require(t.snapshot(s).size == 0);
        t.update(1, &write, 0, nullptr);
        t.destroy_buffer(b); require(t.snapshot(s).size == 0);
        t.create_buffer(b, 256); require(t.snapshot(s).size == 0);
        t.bind(b, m, 128); require(t.snapshot(s).size == 224);
        t.free(m); require(t.snapshot(s).size == 0);
        t.allocate(m, 1024, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        require(t.snapshot(s).size == 0);
        t.map(m, 0, 512, reinterpret_cast<void*>(1));
        require(t.snapshot(s).size == 0); // ReadProcessMemory reports an inaccessible address.
        t.free_sets(1, &s); require(t.snapshot(s).size == 0);
        std::cout << checks << " mapped-memory safety checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
