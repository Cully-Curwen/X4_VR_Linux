#include <x4vr/copy_call_hook.hpp>
#include <x4vr/scene_camera_guard.hpp>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace x4vr::experimental;
extern "C" void* copy_fixture(void*, const void*, size_t);
extern "C" void* copy_fixture_other(void*, const void*, size_t);
extern "C" char copy_fixture_site;
std::atomic_uint64_t originals{};
extern "C" __declspec(noinline) void* copy_fixture_original(void* dst, const void* src, size_t size) {
    ++originals;
    std::memcpy(dst, src, size);
    return dst;
}
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class F> void rejected(F f) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    check(caught, "Expected rejection");
}
enum class Mode { observe, change, exception, recursive };
struct Context { std::atomic_uint64_t callbacks{}; Mode mode{}; };
void observer(void* dst, const void* src, size_t size, void* opaque) {
    auto& ctx = *static_cast<Context*>(opaque);
    ++ctx.callbacks;
    check(std::memcmp(dst, src, size) == 0, "Observer ran before original copy");
    if (ctx.mode == Mode::exception) throw std::runtime_error("injected");
    if (ctx.mode == Mode::recursive) {
        std::array<char, 16> nested{};
        check(copy_fixture(nested.data(), src, size) == nested.data(), "Nested return");
        check(std::memcmp(nested.data(), src, size) == 0, "Nested copy");
    }
    if (ctx.mode == Mode::change && size) static_cast<char*>(dst)[0] = 'X';
}
int main() {
    try {
        constexpr uintptr_t pool = 0x10000000, stride = 0xf80;
        for (uint32_t read_half : {0u, 1u}) {
            const auto first = pool+static_cast<uintptr_t>(read_half ^ 1)*100*stride+0x10;
            check(scene_camera_destination(first, 0xd10, pool, read_half, 1, "class U::Zone"), "Write-half slot rejected");
            check(scene_camera_destination(first+99*stride, 0xd10, pool, read_half, 100, "class U::Zone"), "Last slot rejected");
            check(!scene_camera_destination(first+stride, 0xd10, pool, read_half, 1, "class U::Zone"), "Unproduced slot accepted");
            check(!scene_camera_destination(first+1, 0xd10, pool, read_half, 1, "class U::Zone"), "Misaligned slot accepted");
            check(!scene_camera_destination(first, 64, pool, read_half, 1, "class U::Zone"), "Wrong size accepted");
            check(!scene_camera_destination(first, 0xd10, pool, read_half, 1, "Anark - monitors"), "UI accepted");
            check(!scene_camera_destination(first, 0xd10, pool, read_half ^ 1, 100, "class U::Zone"), "Read half accepted");
            check(!scene_camera_destination(first, 0xd10, pool, read_half, 101, "class U::Zone"), "Unbounded count accepted");
        }
        check(!scene_camera_destination(pool-1, 0xd10, pool, 0, 1, "class U::Zone"), "Underflow accepted");
        check(!scene_camera_destination(pool+0x10, 0xd10, pool, 2, 1, "class U::Zone"), "Invalid half accepted");
        check(!scene_camera_destination(UINTPTR_MAX, 0xd10, UINTPTR_MAX-0x10, 1, 1, "class U::Zone"), "Overflow pool accepted");
        std::array<std::byte, 5> saved{};
        std::memcpy(saved.data(), &copy_fixture_site, saved.size());
        CopyCallHook hook;
        Context context;
        auto install = [&] { hook.install_quiescent(&copy_fixture_site, copy_fixture_original, observer, &context); };
        rejected([&] { hook.install_quiescent(nullptr, copy_fixture_original, observer, &context); });
        rejected([&] { hook.install_quiescent(reinterpret_cast<void*>(1), copy_fixture_original, observer, &context); });
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        rejected([&] { hook.install_quiescent(reinterpret_cast<void*>(static_cast<uintptr_t>(info.dwPageSize-2)), copy_fixture_original, observer, &context); });
        rejected([&] { hook.install_quiescent(&copy_fixture_site, copy_fixture_other, observer, &context); });
        rejected([&] { hook.install_quiescent(&copy_fixture_site, copy_fixture_original, nullptr, &context); });
        const std::array<char, 16> source{'a','b','c'};
        std::array<char, 16> target{};
        install();
        check(hook.installed(), "Hook not installed");
        rejected(install);
        { CopyCallHook other; rejected([&] { other.install_quiescent(&copy_fixture_site, copy_fixture_original, observer, &context); }); }
        for (const auto mode : {Mode::observe, Mode::change, Mode::exception, Mode::recursive}) {
            context.mode = mode;
            const auto before = originals.load();
            const auto calls = context.callbacks.load();
            check(copy_fixture(target.data(), source.data(), source.size()) == target.data(), "Return value changed");
            check(originals == before+(mode == Mode::recursive ? 2u : 1u), "Original count");
            check(context.callbacks == calls+1, "Recursive observer entered");
            check(target[0] == (mode == Mode::change ? 'X' : 'a'), "Copy result differs");
            check(source[0] == 'a', "Source mutated");
            copy_fixture_other(target.data(), source.data(), source.size());
            check(target == source && context.callbacks == calls+1, "Other call site intercepted");
        }
        context.mode = Mode::observe;
        const auto before = context.callbacks.load();
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 8; ++worker) workers.emplace_back([&] {
            std::array<char, 16> output{};
            for (int i = 0; i < 1000; ++i) copy_fixture(output.data(), source.data(), source.size());
        });
        for (auto& worker : workers) worker.join();
        check(context.callbacks == before+8000, "Concurrent copies lost");
        bool wrong_thread_rejected{};
        std::thread wrong([&] { try { hook.remove_quiescent(); } catch (...) { wrong_thread_rejected = true; } });
        wrong.join();
        check(wrong_thread_rejected && hook.installed(), "Wrong-thread removal accepted");
        hook.remove_quiescent();
        check(!hook.installed(), "Hook still installed");
        check(std::memcmp(&copy_fixture_site, saved.data(), saved.size()) == 0, "Original code not restored");
        const auto after = context.callbacks.load();
        copy_fixture(target.data(), source.data(), source.size());
        check(target == source && context.callbacks == after, "Callback after removal");
        install(); hook.remove_quiescent();
        std::cout << "Scoped copy call, passthrough, recursion, concurrency, restoration passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
