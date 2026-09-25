#pragma once
#include <array>
#include <cstddef>

namespace x4vr::experimental {
using CopyFunction = void* (*)(void*, const void*, size_t);
// Called AFTER the original synchronous copy. Exceptions are contained, but
// callbacks own any mutations and must not throw after a partial mutation.
using CopyObserver = void (*)(void*, const void*, size_t, void*);

// Replaces one E8 rel32 call through an owned nearby absolute-jump relay.
// Does NOT detour the copy function globally. All callers must be quiescent
// for install/remove/destruction; same managing thread, never DllMain.
// One live owner per library. Installed code/context must outlive every caller.
class CopyCallHook final {
public:
    CopyCallHook() = default;
    ~CopyCallHook() noexcept;
    CopyCallHook(const CopyCallHook&) = delete;
    CopyCallHook& operator=(const CopyCallHook&) = delete;
    void install_quiescent(void* call_site, CopyFunction expected_target,
                           CopyObserver observer, void* context);
    void remove_quiescent();
    bool installed() const noexcept { return patched_; }
private:
    static void* dispatch(void*, const void*, size_t);
    void* site_{};
    void* relay_{};
    CopyFunction original_{};
    CopyObserver observer_{};
    void* context_{};
    unsigned long owner_thread_{};
    unsigned long protection_{};
    std::array<std::byte, 5> saved_{}, replacement_{};
    bool patched_{};
};
}
