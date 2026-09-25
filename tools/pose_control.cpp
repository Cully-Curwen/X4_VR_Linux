#include <x4vr/pose_control.hpp>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    using namespace x4vr::experimental;
    if (argc != 3) { std::cerr << "Usage: pose_control <X4-pid> <enable|disable|recenter|shutdown>\n"; return 2; }
    DWORD pid{};
    const std::string_view argument = argv[1], action = argv[2];
    const auto parsed = std::from_chars(argument.data(), argument.data()+argument.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data()+argument.size() || !pid) return 2;
    PoseCommand command = PoseCommand::none;
    if (action == "enable") command = PoseCommand::enable;
    else if (action == "disable") command = PoseCommand::disable;
    else if (action == "recenter") command = PoseCommand::recenter;
    else if (action == "shutdown") command = PoseCommand::shutdown;
    if (command == PoseCommand::none) return 2;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) { std::cerr << "X4 process unavailable\n"; return 3; }
    wchar_t path[32768]{}; DWORD length = 32768;
    const bool found = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!found || _wcsicmp(std::filesystem::path(path).filename().c_str(), L"X4.exe")) {
        std::cerr << "Target is not X4.exe\n"; return 3;
    }
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, pose_event_name(pid, command).c_str());
    if (!event) { std::cerr << "No head-look controls for this X4 process\n"; return 4; }
    const bool sent = SetEvent(event) != FALSE;
    CloseHandle(event);
    if (!sent) return 5;
    std::cout << "Requested " << action << "; applies at the next selected-camera update. Check the native log for acknowledgment.\n";
}
