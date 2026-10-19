// Starting X4 in VR: X4's Steam launch option (x4vr-run %command%) and Launch in VR from the menu.

#pragma once

#include "launch_option.hpp"
#include <functional>
#include <string>

namespace x4vr::linux_launcher {

// X4's launch option for this install: x4vr-run %command%.
std::string make_launch_option();

// X4's launch option in every Steam account, judged against this install's (launch_option.hpp).
x4vr::linux_port::LaunchOption read_launch_option();

// Whether SteamVR runs (its vrserver process).
bool is_steamvr_running();

// Launch in VR: SteamVR (started if needed, waiting for it), then
// the request for x4vr-run and `steam -applaunch`. `progress` shows a line; `cancelled` is polled
// while waiting. True once X4 runs.
bool play(const std::function<void(const std::string&)>& progress, const std::function<bool()>& cancelled);

} // namespace x4vr::linux_launcher
