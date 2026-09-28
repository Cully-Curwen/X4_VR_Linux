// The background thread behind stereo_settings, write_file_later and take_request: settings
// reload, in-order writes and request polling, in a temporary capture folder.
#include <x4vr/runtime_bootstrap.hpp>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> bool eventually(F done) { // polled every 500 ms: allow 3 s
    for (int i = 0; i < 60; ++i, std::this_thread::sleep_for(std::chrono::milliseconds(50))) if (done()) return true;
    return false;
}
std::string read(const fs::path& path) { std::ifstream in(path); std::stringstream s; s << in.rdbuf(); return s.str(); }
int main() try {
    const auto root = fs::temp_directory_path()/("x4vr-background-"+std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    SetEnvironmentVariableW(L"X4VR_CAPTURE_DIR", root.wstring().c_str());
    std::ofstream(root/"stereo.txt") << "submit_budget_ms=3.5\nhandoff=1\nrelease_late=1\ndelay_walk=3\n";
    const auto first = x4vr::stereo_settings(); // the first call loads synchronously
    check(first.submit_budget_ms == 3.5f && first.handoff && first.release_late && first.delay_walk == 3, "settings loaded on first call");
    check(!x4vr::take_request("probe.request"), "no request before the file exists");
    std::ofstream(root/"stereo.txt") << "submit_budget_ms=1\n";
    std::ofstream(root/"probe.request") << "go";
    x4vr::write_file_later(root/"out.txt", "a\n", false);
    x4vr::write_file_later(root/"out.txt", "b\n", true);
    x4vr::write_file_later(root/"out.txt", "c\n", true);
    check(eventually([] { return x4vr::take_request("probe.request"); }), "request seen");
    check(!fs::exists(root/"probe.request"), "request file deleted");
    check(!x4vr::take_request("probe.request"), "request taken once");
    check(eventually([] { const auto s = x4vr::stereo_settings(); return s.submit_budget_ms == 1 && !s.handoff && !s.release_late; }), "settings reloaded");
    check(eventually([&] { return read(root/"out.txt") == "a\nb\nc\n"; }), "writes in order, first one replaces");
    x4vr::write_file_later(root/"out.txt", "d\n", false);
    check(eventually([&] { return read(root/"out.txt") == "d\n"; }), "replace after appends");
    std::error_code ignored; fs::remove_all(root, ignored);
    std::cout << "background tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
