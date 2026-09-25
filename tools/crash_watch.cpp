// External crash recorder. Read-only unless --startup-module is explicitly used
// for this recorder's newly created child; never attaches a module to a live game.
#include <windows.h>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <array>
#include <sstream>
#include "startup_module.hpp"

struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void module(std::ofstream& log, const char* label, const void* base, HANDLE file) {
    wchar_t path[32768]{};
    if (file) GetFinalPathNameByHandleW(file, path, 32768, FILE_NAME_NORMALIZED);
    log << label << " base=0x" << std::hex << reinterpret_cast<uintptr_t>(base) << std::dec
        << " path=" << std::filesystem::path(path).generic_string() << '\n';
    if (file) CloseHandle(file);
}
// Optional hardware watchpoints, armed from <report>/watch.txt lines
// "0x<address> <r|w>" (max 4, 8-byte aligned). Logs accessor RIP counts.
struct Watch {
    std::map<DWORD, HANDLE> threads;
    std::array<uintptr_t, 4> address{};
    std::array<bool, 4> write_only{};
    std::map<std::pair<int, uintptr_t>, unsigned> hits;
    unsigned total{};
    bool armed{};
    ULONGLONG checked{}, reported{};
    void apply(HANDLE thread) const {
        const bool suspend = SuspendThread(thread) != DWORD(-1);
        CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
            context.Dr0 = address[0]; context.Dr1 = address[1]; context.Dr2 = address[2]; context.Dr3 = address[3];
            DWORD64 dr7 = 0;
            for (int i = 0; i < 4; ++i) if (armed && address[i])
                dr7 |= (1ull << (i*2)) | (DWORD64(write_only[i] ? 1 : 3) << (16+i*4)) | (2ull << (18+i*4));
            context.Dr7 = dr7; context.Dr6 = 0;
            SetThreadContext(thread, &context);
        }
        if (suspend) ResumeThread(thread);
    }
    void poll(const std::filesystem::path& directory, std::ofstream& log) {
        const auto now = GetTickCount64();
        if (now-checked < 500) return;
        checked = now;
        if (armed && now-reported > 2000) { report(log); reported = now; }
        std::ifstream file(directory/"watch.txt");
        if (!file) return;
        std::array<uintptr_t, 4> next{}; std::array<bool, 4> write{};
        std::string line; int count = 0;
        while (count < 4 && std::getline(file, line)) {
            std::istringstream in(line); std::string value, mode;
            if (!(in >> value >> mode)) continue;
            try { next[count] = std::stoull(value, nullptr, 16) & ~uintptr_t(7); } catch (...) { continue; }
            write[count++] = mode == "w";
        }
        file.close();
        std::filesystem::remove(directory/"watch.txt");
        address = next; write_only = write; armed = count > 0; hits.clear(); total = 0;
        for (auto& [id, thread] : threads) apply(thread);
        log << "watch armed=" << count << "\n";
    }
    void report(std::ofstream& log) {
        log << "watch report total=" << total << "\n";
        for (auto& [key, count] : hits)
            log << "  dr" << key.first << " rip=0x" << std::hex << key.second << std::dec << " hits=" << count << "\n";
    }
};
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && !(argc == 5 && std::wstring(argv[3]) == L"--startup-module")) {
        std::cerr << "Usage: crash_watch.exe <executable> <new-report-directory> [--startup-module <dll>]\n"; return 2;
    }
    const auto exe = std::filesystem::absolute(argv[1]);
    const auto directory = std::filesystem::absolute(argv[2]);
    if (!std::filesystem::is_regular_file(exe) || !std::filesystem::create_directory(directory)) {
        std::cerr << "Executable must exist and report directory must be new\n"; return 2;
    }
    std::ofstream log(directory/"debug-events.log");
    if (!log) return 2;
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    // Optional game arguments for the child only (e.g. -skipintro -nocputhrottle).
    if (wchar_t extra[1024]{}; GetEnvironmentVariableW(L"X4VR_GAME_ARGS", extra, 1024) && extra[0])
        command += L" " + std::wstring(extra);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &startup, &process)) {
        log << "create_failed error=" << GetLastError() << '\n'; return 3;
    }
    Handle process_handle{process.hProcess}, initial_thread{process.hThread};
    if (!DebugSetProcessKillOnExit(FALSE)) {
        const auto error = GetLastError();
        DebugActiveProcessStop(process.dwProcessId);
        log << "detach_policy_failed error=" << error << '\n'; return 3;
    }
    log << "started pid=" << process.dwProcessId << " tick=" << GetTickCount64() << '\n'; log.flush();
    std::cout << "Debug child PID: " << process.dwProcessId << std::endl;
    bool initial_breakpoint = true;
    Watch watch;
    unsigned exceptions = 0;
    size_t debug_bytes = 0;
    try {
    std::unique_ptr<StartupModule> startup_module;
    if (argc == 5) startup_module = std::make_unique<StartupModule>(process.hProcess, process.hThread,
        process.dwProcessId, process.dwThreadId, argv[4], log);
    for (;;) {
        if (startup_module) startup_module->check_timeout();
        watch.poll(directory, log);
        DEBUG_EVENT event{};
        if (!WaitForDebugEventEx(&event, 1000)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            log << "wait_failed error=" << GetLastError() << '\n';
            DebugActiveProcessStop(process.dwProcessId); return 4;
        }
        DWORD continuation = DBG_CONTINUE;
        bool exited = false;
        DWORD exit_code = 0;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            watch.threads[event.dwThreadId] = event.u.CreateProcessInfo.hThread;
            if (startup_module) startup_module->process_created(event.u.CreateProcessInfo.lpBaseOfImage);
            module(log, "process", event.u.CreateProcessInfo.lpBaseOfImage, event.u.CreateProcessInfo.hFile);
        } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            watch.threads[event.dwThreadId] = event.u.CreateThread.hThread;
            if (watch.armed) watch.apply(event.u.CreateThread.hThread);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
                   event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP &&
                   watch.threads.count(event.dwThreadId)) {
            // Data breakpoints trap after the access; ExceptionAddress is the next instruction.
            const HANDLE thread = watch.threads[event.dwThreadId];
            CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context) && (context.Dr6 & 0xf)) {
                for (int i = 0; i < 4; ++i) if (context.Dr6 & (1ull << i))
                    ++watch.hits[{i, reinterpret_cast<uintptr_t>(event.u.Exception.ExceptionRecord.ExceptionAddress)}];
                context.Dr6 = 0;
                if (++watch.total >= 20000) { watch.armed = false; context.Dr7 = 0; watch.report(log); log << "watch auto-disarmed\n"; for (auto& [id, other] : watch.threads) if (other != thread) watch.apply(other); }
                SetThreadContext(thread, &context);
            } else continuation = DBG_EXCEPTION_NOT_HANDLED;
        } else if (event.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT) {
            watch.threads.erase(event.dwThreadId);
            if (startup_module) startup_module->thread_exited(event);
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT)
            module(log, "module", event.u.LoadDll.lpBaseOfDll, event.u.LoadDll.hFile);
        else if (event.dwDebugEventCode == OUTPUT_DEBUG_STRING_EVENT && debug_bytes < 4*1024*1024) {
            const auto& info = event.u.DebugString;
            const size_t length = size_t(info.nDebugStringLength)*(info.fUnicode ? sizeof(wchar_t) : 1);
            if (length && length <= 65536) {
                std::vector<char> text(length+sizeof(wchar_t), 0);
                SIZE_T copied{};
                if (ReadProcessMemory(process_handle.value, info.lpDebugStringData, text.data(), length, &copied)) {
                    log << "debug_string tid=" << event.dwThreadId << " text=";
                    if (info.fUnicode) {
                        const int characters = static_cast<int>(copied/sizeof(wchar_t));
                        const auto* wide = reinterpret_cast<const wchar_t*>(text.data());
                        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, wide, characters, nullptr, 0, nullptr, nullptr);
                        if (bytes > 0) {
                            std::string utf8(static_cast<size_t>(bytes), '\0');
                            WideCharToMultiByte(CP_UTF8, 0, wide, characters, utf8.data(), bytes, nullptr, nullptr);
                            log << utf8.c_str();
                        }
                    } else log << text.data();
                    log << '\n'; debug_bytes += copied;
                }
            }
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto record = event.u.Exception.ExceptionRecord;
            const bool first = event.u.Exception.dwFirstChance != 0;
            if (startup_module && startup_module->exception(event)) {
                if (record.ExceptionCode == EXCEPTION_BREAKPOINT) initial_breakpoint = false;
            } else if (initial_breakpoint && first && record.ExceptionCode == EXCEPTION_BREAKPOINT) {
                initial_breakpoint = false;
            } else {
                continuation = DBG_EXCEPTION_NOT_HANDLED;
                if (++exceptions <= 1024 || !first) {
                    log << "exception tick=" << GetTickCount64() << " tid=" << event.dwThreadId
                        << " first=" << first << " code=0x" << std::hex << record.ExceptionCode
                        << " address=0x" << reinterpret_cast<uintptr_t>(record.ExceptionAddress) << std::dec << '\n';
                }
                if (!first) {
                    CONTEXT context{}; context.ContextFlags = CONTEXT_ALL;
                    Handle thread{OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId)};
                    const bool have_context = thread.value && GetThreadContext(thread.value, &context);
                    EXCEPTION_POINTERS pointers{&record, &context};
                    MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId, &pointers, FALSE};
                    const auto path = directory/("crash-" + std::to_string(exceptions) + ".dmp");
                    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
                    const bool ok = file.value != INVALID_HANDLE_VALUE && MiniDumpWriteDump(process_handle.value,
                        process.dwProcessId, file.value, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                        have_context ? &info : nullptr, nullptr, nullptr);
                    log << "dump success=" << ok << " context=" << have_context << " error=" << (ok ? 0 : GetLastError()) << '\n';
                    std::cout << "Unhandled exception captured; dump success=" << ok << std::endl;
                }
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            exited = true; exit_code = event.u.ExitProcess.dwExitCode;
            log << "exit code=" << exit_code << " hex=0x" << std::hex << exit_code << std::dec << '\n';
        }
        log.flush();
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation)) {
            log << "continue_failed error=" << GetLastError() << '\n';
            DebugActiveProcessStop(process.dwProcessId); return 4;
        }
        if (exited) return exit_code == 0 ? 0 : 1;
    }
    } catch (const std::exception& error) {
        log << "startup_module_failed: " << error.what() << '\n'; log.flush();
        std::cerr << error.what() << '\n';
        // This is only our newly launched child, stopped before its entry point.
        // Never leave it suspended with an unresolved startup operation. Do not
        // free remote arguments while a helper might still be reading them.
        TerminateProcess(process.hProcess, 5);
        DebugActiveProcessStop(process.dwProcessId);
        return 5;
    }
}
