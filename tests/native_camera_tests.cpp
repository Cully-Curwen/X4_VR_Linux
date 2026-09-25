#include "native_camera.hpp"
#include <intrin.h>
#include <iostream>

__declspec(noinline) bool probe() {
    const auto address = reinterpret_cast<uintptr_t>(_ReturnAddress());
    CONTEXT found{};
    const bool ok = x4vr::observe::context_at_return(address, found);
    return ok && found.Rip == address && found.Rsp != 0;
}
int main() {
    if (!probe()) { std::cerr << "Could not reconstruct caller\n"; return 1; }
    CONTEXT absent{};
    if (x4vr::observe::context_at_return(1, absent)) return 2;
    if (x4vr::observe::supported_executable()) return 3;
    if (x4vr::observe::capture_native_camera().camera_read) return 4;
    std::cout << "Caller reconstruction and unsupported-image rejection passed\n";
}
