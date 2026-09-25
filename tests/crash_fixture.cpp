#include <windows.h>
int main() {
    OutputDebugStringW(L"x4vr diagnostic fixture unicode");
    // Deliberately unhandled only inside this disposable child test executable.
    RaiseException(0xe0425844, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 1;
}
