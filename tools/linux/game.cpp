#include "game.hpp"
#include "steam_config.hpp"
#include <cstdlib>
#include <system_error>
#include <vector>

namespace x4vr::linux_launcher {

std::filesystem::path x4_config() {
    const char* home = std::getenv("HOME");
    const char* xdg = std::getenv("XDG_CONFIG_HOME");

    std::vector<std::filesystem::path> bases{std::filesystem::path(home ? home : ".")/".config/EgoSoft/X4"};
    if (xdg && *xdg) bases.push_back(std::filesystem::path(xdg)/"EgoSoft/X4");

    std::filesystem::path best;
    std::filesystem::file_time_type newest{};
    for (const auto& base : bases) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
            std::error_code time_error;
            const auto config = entry.path()/"config.xml";
            const auto time = std::filesystem::last_write_time(config, time_error);
            if (!time_error && (best.empty() || time > newest)) { best = config; newest = time; }
        }
    }
    return best;
}

std::filesystem::path game_dir() {
    if (const char* dir = std::getenv("X4VR_GAME_DIR"); dir && *dir) return dir;
    for (const auto& library : x4vr::steam::libraries()) // every Steam library, also on other drives
        if (const auto candidate = library/"steamapps/common/X4 Foundations"; std::filesystem::exists(candidate/"01.cat")) return candidate;
    return {};
}

int x4_pid() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        std::error_code link_error;
        if (std::filesystem::read_symlink(entry.path()/"exe", link_error).filename() == "X4") return std::atoi(entry.path().filename().c_str());
    }
    return 0;
}

bool x4_running() { return x4_pid() != 0; }

} // namespace x4vr::linux_launcher
