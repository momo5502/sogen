#include <windows.h>
#include <stdio.h>

void instrumentation_callback(void);

typedef LONG(NTAPI* set_information_process_t)(HANDLE, ULONG, PVOID, ULONG);

typedef struct
{
    ULONG Version;
    ULONG Reserved;
    PVOID Callback;
} instrumentation_callback_information;

#define PROCESS_INSTRUMENTATION_CALLBACK 40

static void enable_debug_privilege(void)
{
    HANDLE token = NULL;
    TOKEN_PRIVILEGES privileges = {1};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token))
    {
        if (LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &privileges.Privileges[0].Luid))
        {
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
        }
        CloseHandle(token);
    }
}

int main(void)
{
    const set_information_process_t set_information_process =
        (set_information_process_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationProcess");
    volatile ULONG64 returns = 0;
    instrumentation_callback_information information = {0, 0, (PVOID)instrumentation_callback};
    LONG status = 0;

    enable_debug_privilege();
    ((NT_TIB*)NtCurrentTeb())->ArbitraryUserPointer = (PVOID)&returns;

    status = set_information_process(GetCurrentProcess(), PROCESS_INSTRUMENTATION_CALLBACK, &information, sizeof(information));
    if (status != 0)
    {
        printf("Setting the instrumentation callback failed: 0x%08lx\n", (unsigned long)status);
        return 1;
    }

    SwitchToThread();
    Sleep(0);

    information.Callback = NULL;
    set_information_process(GetCurrentProcess(), PROCESS_INSTRUMENTATION_CALLBACK, &information, sizeof(information));
    ((NT_TIB*)NtCurrentTeb())->ArbitraryUserPointer = NULL;

    // The callback runs for the return of the call that installed it and of each later system call.
    printf("System call returns observed: %s\n", returns >= 3 ? "yes" : "no");
    return returns >= 3 ? 0 : 2;
}
