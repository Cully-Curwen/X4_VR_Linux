#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>

// Optional startup-only module bootstrap for a child owned by crash_watch.
// Stops the primary thread at the PE entry point AFTER loader initialization,
// loads the module, then calls its exported X4VR_Startup outside DllMain. There
// is no attach-to-running-process path. Startup errors abort this new child.
class StartupModule {
public:
    StartupModule(HANDLE process, HANDLE primary, DWORD pid, DWORD tid,
                  const std::filesystem::path& dll, std::ofstream& log);
    void process_created(void* image);
    bool exception(const DEBUG_EVENT& event);
    void thread_exited(const DEBUG_EVENT& event);
    void check_timeout();
    bool complete() const noexcept { return state_ == State::complete; }
private:
    enum class State { waiting, armed, loading, initializing, complete } state_ = State::waiting;
    HANDLE process_{}, primary_{};
    DWORD pid_{}, tid_{}, helper_tid_{};
    std::filesystem::path dll_;
    std::ofstream& log_;
    uintptr_t entry_{};
    DWORD export_rva_{};
    unsigned char entry_byte_{};
    void* remote_path_{};
    ULONGLONG deadline_{};
    uintptr_t module_base(const std::filesystem::path& path);
    void start_thread(uintptr_t function, void* argument);
};
