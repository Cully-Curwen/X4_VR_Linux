#include "common.hpp"
#include "steam_config.hpp"
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace x4vr::linux_launcher {

std::filesystem::path settings_file() {
    if (const char* dir = std::getenv("X4VR_DIR"); dir && *dir) return std::filesystem::path(dir)/"stereo.txt";
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) return std::filesystem::path(state)/"x4vr/stereo.txt";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".")/".local/state/x4vr/stereo.txt";
}

std::filesystem::path state_dir() { return settings_file().parent_path(); }

std::filesystem::path settings_marker() { return state_dir()/"x4-settings.vr"; }

std::string read_text(const std::filesystem::path& path) { return x4vr::steam::read_file(path); }

bool write_text(const std::filesystem::path& path, const std::string& text) {
    const auto temporary = path.string()+".x4vr-tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

} // namespace x4vr::linux_launcher
