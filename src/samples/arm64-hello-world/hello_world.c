#include <windows.h>

void mainCRTStartup(void)
{
    static const char message[] = "Hello, world!\r\n";
    DWORD written = 0;
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, NULL) || written != sizeof(message) - 1)
    {
        ExitProcess(1);
    }
    ExitProcess(0);
}
