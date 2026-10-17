// X4's settings (its config.xml) for VR: the checks, fix-settings, and the resolution X4 should
// run at in VR.

#pragma once

#include "../launcher/launcher_settings.hpp"
#include <string>
#include <vector>

namespace x4vr::linux_launcher {

// The Windows launcher's checks (tools/launcher/launcher_settings.hpp, tested in launcher_tests),
// minus "fullscreen, not borderless": on Windows that is for NVIDIA DSR; on Linux X4's borderless
// window at the desktop resolution is fine.
std::vector<x4vr::launcher::Check> x4_checks(const std::string& xml);

// Fixes X4's VR settings for VR; `quiet` (x4vr-run): prints only what it changed and errors, no table.
int fix_x4_settings(bool quiet);

} // namespace x4vr::linux_launcher
