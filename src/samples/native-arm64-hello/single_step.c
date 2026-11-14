#include <windows.h>

typedef LONG(NTAPI* nt_continue_t)(PCONTEXT, BOOLEAN);

static volatile LONG steps;
static volatile ULONG64 expected_pc;
static volatile ULONG64 result;

__attribute__((naked, noinline)) static void stepped_sequence(void)
{
    __asm__ volatile("add x0, x0, #1\n"
                     "add x0, x0, #1\n"
                     "str x0, [x1]\n"
                     "ret\n");
}

static LONG CALLBACK handle_single_step(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if ((ULONG_PTR)exception->ExceptionRecord->ExceptionAddress != expected_pc || exception->ContextRecord->Pc != expected_pc ||
        (exception->ContextRecord->Cpsr & 0x200000) != 0 || exception->ContextRecord->X[0] != 0x1235)
    {
        ExitProcess(3);
    }
    ++steps;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void mainCRTStartup(void)
{
    const nt_continue_t nt_continue = (nt_continue_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtContinue");
    PVOID handler = AddVectoredExceptionHandler(1, handle_single_step);
    if (!nt_continue || !handler)
    {
        ExitProcess(1);
    }
    CONTEXT context;
    volatile BOOL resumed = FALSE;
    RtlCaptureContext(&context);
    if (!resumed)
    {
        resumed = TRUE;
        context.X[30] = context.Pc;
        context.Pc = (ULONG_PTR)&stepped_sequence;
        context.X[0] = 0x1234;
        context.X[1] = (ULONG_PTR)&result;
        context.Cpsr |= 0x200000;
        expected_pc = context.Pc + 4;
        nt_continue(&context, FALSE);
        ExitProcess(2);
    }
    const ULONG removed = RemoveVectoredExceptionHandler(handler);
    ExitProcess(steps == 1 && result == 0x1236 && removed != 0 ? 0 : 4);
}
