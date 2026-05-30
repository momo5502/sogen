#include <windows.h>

int main()
{
    const auto result = MessageBoxA(nullptr, "Hello world", "Hello world", MB_OK | MB_ICONINFORMATION);
    return result == IDOK ? 0 : 1;
}
