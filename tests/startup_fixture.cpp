#include <windows.h>
#ifdef X4VR_STARTUP_DLL
extern "C" __declspec(dllexport) DWORD WINAPI X4VR_Startup(void*) {
    OutputDebugStringA("X4VR startup fixture: exported initialization outside DllMain\n");
    wchar_t failure[2]{};
    if (GetEnvironmentVariableW(L"X4VR_STARTUP_TEST_FAIL", failure, 2)) return 73;
    return SetEnvironmentVariableW(L"X4VR_STARTUP_TEST_READY", L"1") ? 0 : 74;
}
#else
int main() {
    OutputDebugStringA("X4VR startup fixture: executable entry reached\n");
    wchar_t ready[2]{};
    return GetEnvironmentVariableW(L"X4VR_STARTUP_TEST_READY", ready, 2) == 1 && ready[0] == L'1' ? 0 : 75;
}
#endif
