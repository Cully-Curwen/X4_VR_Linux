#include "native_camera.hpp"
#include <bcrypt.h>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace x4vr::observe {
namespace {
bool read(uintptr_t address, void* output, size_t size) noexcept {
    SIZE_T count{};
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size, &count) && count == size;
}
struct Hash {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};
bool verify_executable() {
    wchar_t filename[32768]{};
    const auto length = GetModuleFileNameW(nullptr, filename, 32768);
    if (!length || length >= 32768) return false;
    std::ifstream file(std::filesystem::path(filename), std::ios::binary);
    if (!file) return false;
    Hash h;
    if (BCryptOpenAlgorithmProvider(&h.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(h.algorithm, &h.hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    std::array<unsigned char, 65536> bytes{};
    while (file) {
        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (const auto count = file.gcount(); count && BCryptHashData(h.hash, bytes.data(), static_cast<ULONG>(count), 0) < 0) return false;
    }
    if (!file.eof()) return false;
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(h.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr unsigned char pinned[]{0x19,0x75,0x0a,0x65,0x63,0x88,0x9a,0x97,0x0f,0x43,0x4b,0x55,0x66,0xeb,0x39,0x6c,
        0x6b,0x2d,0xc2,0x9f,0xf8,0x14,0xbd,0x3e,0x33,0x6f,0x83,0x81,0x76,0xad,0x68,0x91};
    static_assert(sizeof(pinned) == 32);
    if (std::memcmp(digest.data(), pinned, 32)) return false;
    // Also verify a loaded-image signature at the observed return address.
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    unsigned char signature[9]{};
    constexpr unsigned char wanted[]{0x41,0xc6,0x46,0x54,0x00,0x45,0x88,0x6e,0x55};
    return read(base+0x1218468, signature, sizeof(signature)) && !std::memcmp(signature, wanted, sizeof(wanted));
}
}
bool supported_executable() noexcept {
    try { static const bool supported = verify_executable(); return supported; }
    catch (...) { return false; }
}
// SEH is isolated in a POD-only function. A bad third-party unwind table aborts
// this diagnostic rather than propagating an access violation into the game.
__declspec(noinline) bool context_at_return(uintptr_t address, CONTEXT& output) noexcept {
    __try {
        CONTEXT context{};
        RtlCaptureContext(&context);
        ULONG_PTR low{}, high{};
        GetCurrentThreadStackLimits(&low, &high);
        for (unsigned depth = 0; depth < 64 && context.Rip; ++depth) {
            if (context.Rsp < low || context.Rsp > high-sizeof(uintptr_t)) return false;
            if (context.Rip == address) { output = context; return true; }
            const auto previous_rsp = context.Rsp;
            DWORD64 base{};
            const auto entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (entry) {
                void* handler{}; DWORD64 establisher{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, entry, &context, &handler, &establisher, nullptr);
            } else {
                if (!read(context.Rsp, &context.Rip, sizeof(context.Rip))) return false;
                context.Rsp += sizeof(uintptr_t);
            }
            if (context.Rsp <= previous_rsp) return false;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}
NativeCamera capture_native_camera() noexcept {
    NativeCamera result;
    if (!supported_executable()) { result.status = "unsupported_executable"; return result; }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    CONTEXT context{};
    if (!context_at_return(base+0x1218468, context)) { result.status = "upload_frame_absent"; return result; }
    result.wrapper = context.R14;
    result.temporary = context.Rbp+0x300;
    result.wrapper_read = read(result.wrapper, result.wrapper_bytes.data(), result.wrapper_bytes.size());
    if (result.wrapper_read) {
        std::memcpy(&result.camera, result.wrapper_bytes.data()+8, sizeof(result.camera));
        if (result.camera) result.camera_read = read(result.camera, result.camera_bytes.data(), result.camera_bytes.size());
    }
    ULONG_PTR low{}, high{}; GetCurrentThreadStackLimits(&low, &high);
    if (result.temporary >= low && result.temporary < high && result.uniform_bytes.size() <= high-result.temporary)
        result.uniform_read = read(result.temporary, result.uniform_bytes.data(), result.uniform_bytes.size());
    result.status = result.wrapper_read ? (result.camera_read ? "camera_read" : "camera_null_or_unreadable") : "wrapper_unreadable";
    return result;
}
}
