#include <windows.h>

#include <cstdio>

int main()
{
    const auto result = MessageBoxA(nullptr, "Proceed?", "Question", MB_YESNO | MB_ICONQUESTION);
    if (result == IDYES)
    {
        std::puts("clicked: yes");
        return 0;
    }

    if (result == IDNO)
    {
        std::puts("clicked: no");
        return 1;
    }

    std::printf("clicked: other (%d)\n", result);
    return 2;
}
