#include <x4vr/copy_call_hook.hpp>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace x4vr::experimental {
namespace {
std::atomic<CopyCallHook*> active{};
thread_local bool in_observer{};
bool read_exact(const void* address, void* output, size_t size) {
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(), address, output, size, &read) && read == size;
}
int64_t displacement(const void* site, const void* target) {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(target)) -
           static_cast<int64_t>(reinterpret_cast<uintptr_t>(site)) - 5;
}
bool reachable(const void* site, const void* target) {
    const auto distance = displacement(site, target);
    return distance >= INT32_MIN && distance <= INT32_MAX;
}
void* allocate_relay(void* site) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<uintptr_t>(info.dwAllocationGranularity);
    const auto center = reinterpret_cast<uintptr_t>(site) & ~(granularity - 1);
    // Bounded search, only exact allocations in free regions. No existing
    // allocation is touched; committed memory starts RW and later becomes RX.
    for (uintptr_t distance = 0; distance <= INT32_MAX; distance += granularity) {
        for (int direction : {-1, 1}) {
            if (direction == -1 && distance > center) continue;
            const auto candidate = direction == -1 ? center-distance : center+distance;
            if (candidate < reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress) ||
                candidate > reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress)) continue;
            auto* address = reinterpret_cast<void*>(candidate);
            if (!reachable(site, address)) continue;
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_FREE) continue;
            if (void* result = VirtualAlloc(address, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return result;
        }
    }
    throw std::runtime_error("No free reachable call relay");
}
void protect(void* address, size_t size, DWORD value, DWORD& previous) {
    if (!VirtualProtect(address, size, value, &previous))
        throw std::runtime_error("Call-hook page protection failed");
}
void flush(void* address, size_t size) {
    if (!FlushInstructionCache(GetCurrentProcess(), address, size))
        throw std::runtime_error("Call-hook instruction cache flush failed");
}
}
CopyCallHook::~CopyCallHook() noexcept {
    try { remove_quiescent(); } catch (...) { std::terminate(); }
}
void CopyCallHook::install_quiescent(void* site, CopyFunction expected,
                                    CopyObserver observer, void* context) {
    if (!site || !expected || !observer) throw std::invalid_argument("Missing copy-call hook argument");
    if (site_) throw std::logic_error("Copy-call hook already owned");
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    if (reinterpret_cast<uintptr_t>(site)%info.dwPageSize > info.dwPageSize-5)
        throw std::invalid_argument("Copy-call instruction crosses a protection page");
    std::array<std::byte, 5> code{};
    if (!read_exact(site, code.data(), code.size()) || code[0] != std::byte{0xe8})
        throw std::runtime_error("Expected a readable E8 copy-call instruction");
    int32_t relative{};
    std::memcpy(&relative, code.data()+1, sizeof(relative));
    if (displacement(site, reinterpret_cast<void*>(expected)) != relative)
        throw std::runtime_error("Copy-call target differs from pinned target");
    CopyCallHook* empty{};
    if (!active.compare_exchange_strong(empty, this, std::memory_order_acq_rel))
        throw std::logic_error("Another copy-call hook is owned");
    site_ = site; saved_ = code; original_ = expected;
    observer_ = observer; context_ = context; owner_thread_ = GetCurrentThreadId();
    try {
        relay_ = allocate_relay(site);
        // jmp qword ptr [rip+0], followed by the absolute dispatch address.
        std::array<std::byte, 14> relay_code{std::byte{0xff}, std::byte{0x25}};
        const auto destination = reinterpret_cast<uintptr_t>(&dispatch);
        std::memcpy(relay_code.data()+6, &destination, sizeof(destination));
        std::memcpy(relay_, relay_code.data(), relay_code.size());
        DWORD ignored{};
        protect(relay_, 4096, PAGE_EXECUTE_READ, ignored);
        flush(relay_, relay_code.size());
        replacement_[0] = std::byte{0xe8};
        relative = static_cast<int32_t>(displacement(site, relay_));
        std::memcpy(replacement_.data()+1, &relative, sizeof(relative));
        active.store(this, std::memory_order_release);
        protect(site_, saved_.size(), PAGE_EXECUTE_READWRITE, protection_);
        std::memcpy(site_, replacement_.data(), replacement_.size());
        patched_ = true;
        flush(site_, replacement_.size());
        protect(site_, saved_.size(), protection_, ignored);
    } catch (...) {
        remove_quiescent();
        throw;
    }
}
void CopyCallHook::remove_quiescent() {
    if (active.load(std::memory_order_acquire) != this) return;
    if (GetCurrentThreadId() != owner_thread_)
        throw std::logic_error("Copy-call hook owner thread changed");
    if (patched_) {
        std::array<std::byte, 5> actual{};
        if (!read_exact(site_, actual.data(), actual.size()) || actual != replacement_)
            throw std::runtime_error("Copy-call patch changed; refusing to overwrite another owner");
        DWORD ignored{};
        protect(site_, saved_.size(), PAGE_EXECUTE_READWRITE, ignored);
        std::memcpy(site_, saved_.data(), saved_.size());
        flush(site_, saved_.size());
        protect(site_, saved_.size(), protection_, ignored);
        patched_ = false;
    }
    if (relay_ && !VirtualFree(relay_, 0, MEM_RELEASE))
        throw std::runtime_error("Cannot release owned call relay");
    relay_ = site_ = nullptr;
    original_ = nullptr; observer_ = nullptr; context_ = nullptr;
    active.store(nullptr, std::memory_order_release);
}
void* CopyCallHook::dispatch(void* destination, const void* source, size_t size) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->original_) std::terminate();
    void* result = self->original_(destination, source, size);
    if (!in_observer) {
        in_observer = true;
        try { self->observer_(destination, source, size, self->context_); }
        catch (...) { /* Original copy and return are preserved. */ }
        in_observer = false;
    }
    return result;
}
}
