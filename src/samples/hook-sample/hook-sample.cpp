// Minimal sample for verifying API interception.
//
// Calls GetCurrentProcessId() once and returns 0 iff the value matches the
// hardcoded magic. With the real Windows API this exits non-zero; under sogen
// with an api_call hook returning the magic via ApiContinuation.intercept it
// must exit zero. This isolates the intercept code path from any DLL behavior
// that may depend on a real PID.

#include <windows.h>

constexpr DWORD kExpectedPid = 0xC0FFEE01;

int main()
{
    return GetCurrentProcessId() == kExpectedPid ? 0 : 1;
}
