#include "startup_module.hpp"
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>

namespace {
void checked(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation)+" (error "+std::to_string(GetLastError())+")");
}
template<class T> T remote_read(HANDLE process, uintptr_t address) {
    T value{}; SIZE_T count{};
    checked(ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &count)
            && count == sizeof(value), "Read remote PE header");
    return value;
}
void entry_byte(HANDLE process, uintptr_t address, unsigned char value) {
    auto* pointer = reinterpret_cast<void*>(address);
    DWORD protection{};
    checked(VirtualProtectEx(process, pointer, 1, PAGE_EXECUTE_READWRITE, &protection), "Protect entry breakpoint byte");
    SIZE_T written{};
    const bool copied = WriteProcessMemory(process, pointer, &value, 1, &written) && written == 1;
    DWORD ignored{};
    const bool restored = VirtualProtectEx(process, pointer, 1, protection, &ignored) != FALSE;
    checked(copied && restored && FlushInstructionCache(process, pointer, 1), "Write/restore entry breakpoint byte");
}
// Map bytes as data, never execute the candidate DLL in the recorder process.
DWORD startup_export(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    checked(file != INVALID_HANDLE_VALUE, "Open startup DLL");
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    CloseHandle(file);
    checked(mapping != nullptr, "Map startup DLL image");
    auto* image = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    checked(image != nullptr, "View startup DLL image");
    DWORD found{};
    // Image mapping validates PE layout; every export/table access is additionally
    // bounded by SizeOfImage. Input is a locally built module, not arbitrary code.
    try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew > 0x100000)
            throw std::runtime_error("Invalid DLL DOS header");
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            throw std::runtime_error("Startup DLL must be x64");
        const size_t size = nt->OptionalHeader.SizeOfImage;
        auto range = [&](DWORD rva, size_t bytes) {
            if (!rva || rva >= size || bytes > size-rva) throw std::runtime_error("Invalid DLL export range");
            return image+rva;
        };
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(range(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
        const auto* names = reinterpret_cast<const DWORD*>(range(exports->AddressOfNames, size_t(exports->NumberOfNames)*sizeof(DWORD)));
        const auto* ordinals = reinterpret_cast<const WORD*>(range(exports->AddressOfNameOrdinals, size_t(exports->NumberOfNames)*sizeof(WORD)));
        const auto* functions = reinterpret_cast<const DWORD*>(range(exports->AddressOfFunctions, size_t(exports->NumberOfFunctions)*sizeof(DWORD)));
        constexpr char name[] = "X4VR_Startup";
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            const auto* candidate = range(names[i], sizeof(name));
            if (std::memcmp(candidate, name, sizeof(name))) continue;
            if (ordinals[i] >= exports->NumberOfFunctions) throw std::runtime_error("Invalid DLL export ordinal");
            found = functions[ordinals[i]];
            range(found, 1);
            if (found >= directory.VirtualAddress && found-directory.VirtualAddress < directory.Size)
                throw std::runtime_error("Forwarded startup export is unsupported");
            break;
        }
        if (!found) throw std::runtime_error("DLL has no X4VR_Startup export");
    } catch (...) { UnmapViewOfFile(image); throw; }
    UnmapViewOfFile(image);
    return found;
}
}
StartupModule::StartupModule(HANDLE process, HANDLE primary, DWORD pid, DWORD tid,
    const std::filesystem::path& dll, std::ofstream& log)
    : process_(process), primary_(primary), pid_(pid), tid_(tid), dll_(std::filesystem::canonical(dll)), log_(log) {
    export_rva_ = startup_export(dll_);
    deadline_ = GetTickCount64()+30000;
}
void StartupModule::process_created(void* image) {
    const auto base = reinterpret_cast<uintptr_t>(image);
    const auto dos = remote_read<IMAGE_DOS_HEADER>(process_, base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x100000)
        throw std::runtime_error("Invalid child PE header");
    const auto nt = remote_read<IMAGE_NT_HEADERS64>(process_, base+dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || !nt.OptionalHeader.AddressOfEntryPoint || nt.OptionalHeader.AddressOfEntryPoint >= nt.OptionalHeader.SizeOfImage)
        throw std::runtime_error("Child must have a valid x64 entry point");
    entry_ = base+nt.OptionalHeader.AddressOfEntryPoint;
}
uintptr_t StartupModule::module_base(const std::filesystem::path& path) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid_);
    checked(snapshot != INVALID_HANDLE_VALUE, "Enumerate child modules");
    MODULEENTRY32W item{}; item.dwSize = sizeof(item);
    uintptr_t result{};
    if (Module32FirstW(snapshot, &item)) do {
        std::error_code error;
        if (std::filesystem::equivalent(path, item.szExePath, error) && !error) {
            result = reinterpret_cast<uintptr_t>(item.modBaseAddr); break;
        }
    } while (Module32NextW(snapshot, &item));
    CloseHandle(snapshot);
    if (!result) throw std::runtime_error("Required module absent from child");
    return result;
}
void StartupModule::start_thread(uintptr_t function, void* argument) {
    HANDLE thread = CreateRemoteThread(process_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), argument, 0, &helper_tid_);
    checked(thread != nullptr, "Start startup helper thread");
    CloseHandle(thread);
    deadline_ = GetTickCount64()+30000;
}
bool StartupModule::exception(const DEBUG_EVENT& event) {
    if (event.dwThreadId != tid_ || !event.u.Exception.dwFirstChance) return false;
    const auto& record = event.u.Exception.ExceptionRecord;
    if (state_ == State::waiting && record.ExceptionCode == EXCEPTION_BREAKPOINT) {
        entry_byte_ = remote_read<unsigned char>(process_, entry_);
        if (entry_byte_ == 0xcc) throw std::runtime_error("Child entry already has a breakpoint");
        entry_byte(process_, entry_, 0xcc);
        state_ = State::armed;
        log_ << "startup_module entry_armed address=0x" << std::hex << entry_ << std::dec << '\n';
        return true;
    }
    if (state_ != State::armed || record.ExceptionCode != EXCEPTION_BREAKPOINT
        || reinterpret_cast<uintptr_t>(record.ExceptionAddress) != entry_) return false;
    CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
    checked(GetThreadContext(primary_, &context), "Get stopped entry context");
    if (context.Rip != entry_+1) throw std::runtime_error("Unexpected entry breakpoint instruction pointer");
    entry_byte(process_, entry_, entry_byte_);
    context.Rip = entry_;
    checked(SetThreadContext(primary_, &context), "Restore entry instruction pointer");
    checked(SuspendThread(primary_) != DWORD(-1), "Hold primary at entry");
    const auto path = dll_.wstring();
    const auto bytes = (path.size()+1)*sizeof(wchar_t);
    remote_path_ = VirtualAllocEx(process_, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    checked(remote_path_ != nullptr, "Allocate startup DLL path");
    SIZE_T written{};
    checked(WriteProcessMemory(process_, remote_path_, path.c_str(), bytes, &written) && written == bytes, "Write startup DLL path");
    const auto function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    checked(function != nullptr, "Resolve LoadLibraryW");
    HMODULE owner{};
    checked(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(function), &owner), "Resolve LoadLibraryW owner");
    wchar_t filename[32768]{};
    checked(GetModuleFileNameW(owner, filename, 32768) != 0, "Resolve system module path");
    const auto offset = reinterpret_cast<uintptr_t>(function)-reinterpret_cast<uintptr_t>(owner);
    state_ = State::loading;
    start_thread(module_base(filename)+offset, remote_path_);
    log_ << "startup_module primary_held_at_entry loading tid=" << helper_tid_ << '\n';
    return true;
}
void StartupModule::thread_exited(const DEBUG_EVENT& event) {
    if (event.dwThreadId != helper_tid_) return;
    if (state_ == State::loading) {
        // LoadLibrary's return is a 64-bit HMODULE, NOT its truncated DWORD
        // thread exit code. Confirm the exact loaded module through enumeration.
        const auto base = module_base(dll_);
        checked(VirtualFreeEx(process_, remote_path_, 0, MEM_RELEASE), "Release completed DLL path");
        remote_path_ = nullptr;
        state_ = State::initializing;
        start_thread(base+export_rva_, nullptr);
        log_ << "startup_module loaded base=0x" << std::hex << base << std::dec << " initializing tid=" << helper_tid_ << '\n';
    } else if (state_ == State::initializing) {
        if (event.u.ExitThread.dwExitCode != 0) throw std::runtime_error("Startup module rejected initialization: "+std::to_string(event.u.ExitThread.dwExitCode));
        checked(ResumeThread(primary_) == 1, "Release primary entry hold");
        state_ = State::complete;
        log_ << "startup_module initialized primary_resumed\n";
    }
}
void StartupModule::check_timeout() {
    if (!complete() && GetTickCount64() >= deadline_) throw std::runtime_error("Startup module initialization timed out");
}
