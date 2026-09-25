#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <stdexcept>

namespace x4vr::experimental {
enum class PoseCommand { none, enable, disable, recenter, shutdown };
inline std::wstring pose_event_name(DWORD pid, PoseCommand command) {
    return L"Local\\X4VR.Pose."+std::to_wstring(pid)+L"."+std::to_wstring(static_cast<int>(command));
}
// Only created for explicit head-look mode. Auto-reset events, polled at native
// camera boundaries. Shutdown/disable take precedence over simultaneous enables.
class PoseControls {
    std::array<HANDLE, 4> handles_{};
public:
    explicit PoseControls(DWORD pid) {
        for (int i = 0; i < 4; ++i) {
            handles_[i] = CreateEventW(nullptr, FALSE, FALSE, pose_event_name(pid, static_cast<PoseCommand>(i+1)).c_str());
            const auto error = GetLastError();
            if (!handles_[i] || error == ERROR_ALREADY_EXISTS) {
                for (auto handle : handles_) if (handle) CloseHandle(handle);
                throw std::runtime_error("Head-look control creation failed or name already exists");
            }
        }
    }
    ~PoseControls() { for (auto handle : handles_) if (handle) CloseHandle(handle); }
    PoseControls(const PoseControls&) = delete;
    PoseControls& operator=(const PoseControls&) = delete;
    PoseCommand poll() noexcept {
        std::array<bool, 4> signaled{};
        for (int i = 0; i < 4; ++i) signaled[i] = WaitForSingleObject(handles_[i], 0) == WAIT_OBJECT_0;
        if (signaled[3]) return PoseCommand::shutdown;
        if (signaled[1]) return PoseCommand::disable;
        if (signaled[0]) return PoseCommand::enable;
        if (signaled[2]) return PoseCommand::recenter;
        return PoseCommand::none;
    }
};
}
