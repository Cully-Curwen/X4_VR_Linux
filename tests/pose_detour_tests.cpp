#include <x4vr/pose_detour.hpp>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace x4vr::experimental;
extern "C" uintptr_t pose_fixture(void*, const NativePose*);
struct Camera {
    NativePose pose{};
    uint64_t calls{};
    const NativePose* input{};
};
static_assert(offsetof(Camera, calls) == 0x40 && offsetof(Camera, input) == 0x48);
constexpr std::array signature{std::byte{0x48}, std::byte{0x8b}, std::byte{0xc4},
    std::byte{0x48}, std::byte{0x81}, std::byte{0xec}, std::byte{0x68},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}};
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Function> void rejected(Function&& f) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    check(caught, "Expected rejection");
}
NativePose identity() {
    NativePose pose{};
    pose[0] = pose[5] = pose[10] = pose[15] = 1;
    pose[12] = 31451; pose[13] = 267; pose[14] = -7225;
    return pose;
}
enum class Mode { offset, decline, exception, nonfinite, reflection, recursion };
struct Context {
    std::atomic_uint64_t calls{};
    Mode mode = Mode::offset;
    Camera* selected{};
};
bool transform(const NativePose& in, NativePose& out, void* opaque) {
    auto& context = *static_cast<Context*>(opaque);
    ++context.calls;
    out = in;
    switch (context.mode) {
    case Mode::offset: out[12] += 0.125f; return true;
    case Mode::decline: out[12] += 99; return false;
    case Mode::exception: throw std::runtime_error("Injected transform failure");
    case Mode::nonfinite: out[12] = std::numeric_limits<float>::quiet_NaN(); return true;
    case Mode::reflection: out[0] = -1; return true;
    case Mode::recursion:
        check(pose_fixture(context.selected, &in) == reinterpret_cast<uintptr_t>(context.selected), "Nested return changed");
        out[13] += 0.25f; return true;
    }
    return false;
}
void original_bytes() {
    check(std::memcmp(reinterpret_cast<void*>(&pose_fixture), signature.data(), signature.size()) == 0,
          "Fixture entry bytes differ from expected prologue");
}
int main() {
    try {
        Camera selected{}, other{};
        Context context; context.selected = &selected;
        const auto base = identity();
        original_bytes();
        check(pose_fixture(&selected, &base) == reinterpret_cast<uintptr_t>(&selected), "Baseline return");
        check(selected.pose == base && selected.input == &base && selected.calls == 1, "Baseline copy");
        PoseDetour hook;
        auto wrong = signature; wrong[0] = std::byte{0};
        rejected([&] { hook.install_quiescent(pose_fixture, wrong, &selected, transform, &context); });
        rejected([&] { hook.install_quiescent(pose_fixture, signature, nullptr, transform, &context); });
        rejected([&] { hook.install_quiescent(pose_fixture, signature, &selected, nullptr, &context); });
        rejected([&] { hook.install_quiescent(reinterpret_cast<PoseFunction>(uintptr_t{1}), signature, &selected, transform, &context); });
        original_bytes();
        hook.install_quiescent(pose_fixture, signature, &selected, transform, &context);
        check(hook.installed(), "Hook disabled");
        rejected([&] { hook.install_quiescent(pose_fixture, signature, &selected, transform, &context); });
        {
            PoseDetour second;
            rejected([&] { second.install_quiescent(pose_fixture, signature, &other, transform, &context); });
        }
        check(pose_fixture(&selected, &base) == reinterpret_cast<uintptr_t>(&selected), "Hook return changed");
        check(selected.pose[12] == base[12]+0.125f && selected.calls == 2, "Selected pose not transformed exactly once");
        check(base == identity(), "Input modified");
        const auto callback_count = context.calls.load();
        pose_fixture(&other, &base);
        check(other.pose == base && other.input == &base && context.calls == callback_count, "Other camera changed");
        for (auto mode : {Mode::decline, Mode::exception, Mode::nonfinite, Mode::reflection}) {
            context.mode = mode;
            pose_fixture(&selected, &base);
            check(selected.pose == base && selected.input == &base, "Rejected transform did not preserve input pointer");
        }
        auto malformed = base; malformed[0] = 2;
        const auto before_invalid = context.calls.load();
        pose_fixture(&selected, &malformed);
        check(selected.pose == malformed && selected.input == &malformed && context.calls == before_invalid,
              "Invalid native input was altered");
        context.mode = Mode::recursion;
        const auto before_nested = selected.calls;
        const auto before_callback = context.calls.load();
        pose_fixture(&selected, &base);
        check(selected.calls == before_nested+2 && context.calls == before_callback+1 && selected.pose[13] == base[13]+0.25f,
              "Recursive callback guard failed");
        std::atomic_bool wrong_thread_rejected{};
        std::thread wrong_thread([&] {
            try { hook.remove_quiescent(); } catch (const std::logic_error&) { wrong_thread_rejected = true; }
        });
        wrong_thread.join();
        check(wrong_thread_rejected && hook.installed(), "Wrong-thread removal accepted");
        context.mode = Mode::offset;
        std::atomic_bool concurrent_ok{true};
        std::vector<std::thread> workers;
        for (int i = 0; i < 3; ++i) workers.emplace_back([&] {
            Camera camera;
            for (int j = 0; j < 2000; ++j) {
                if (pose_fixture(&camera, &base) != reinterpret_cast<uintptr_t>(&camera) || camera.pose != base || camera.input != &base)
                    concurrent_ok = false;
            }
            if (camera.calls != 2000) concurrent_ok = false;
        });
        for (int i = 0; i < 2000; ++i) {
            pose_fixture(&selected, &base);
            check(selected.pose[12] == base[12]+0.125f, "Concurrent selected pose failed");
        }
        for (auto& worker : workers) worker.join(); // required quiescence, not just an in-flight counter
        check(concurrent_ok, "Concurrent pass-through failed");
        hook.remove_quiescent();
        hook.remove_quiescent();
        original_bytes();
        pose_fixture(&selected, &base);
        check(selected.pose == base && selected.input == &base, "Removed hook still active");
        {
            PoseDetour reinstall;
            reinstall.install_quiescent(pose_fixture, signature, &selected, transform, &context);
            pose_fixture(&selected, &base);
            check(selected.pose[12] == base[12]+0.125f, "Reinstall failed");
        } // no callers remain: destructor teardown is safe in this fixture
        original_bytes();
        std::cout << "Pose trampoline: selected-camera filtering, input/return preservation, rejection, recursion, "
                     "8000 concurrent calls, owner-thread removal and reinstall passed. No X4 hook installed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
