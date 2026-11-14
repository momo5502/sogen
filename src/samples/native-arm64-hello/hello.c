#include <windows.h>

void mainCRTStartup(void)
{
    static const char message[] = "Hello World\n";
    DWORD written = 0;
    const BOOL success = WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, NULL);
    ExitProcess(success && written == sizeof(message) - 1 ? 0 : 1);
}
