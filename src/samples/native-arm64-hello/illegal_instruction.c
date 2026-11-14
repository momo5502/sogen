#include <windows.h>

extern void current_el_fault(void);

static volatile LONG handled;

__attribute__((naked, noinline)) static ULONG64 probe_current_el(void)
{
    __asm__ volatile("mov x16, #0x1234\n"
                     ".global current_el_fault\n"
                     "current_el_fault:\n"
                     "mrs x16, CurrentEL\n"
                     "mov x0, x16\n"
                     "ret\n");
}

static LONG CALLBACK handle_illegal_instruction(EXCEPTION_POINTERS* exception)
{
    const ULONG64 fault_pc = (ULONG_PTR)&current_el_fault;
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION ||
        (ULONG_PTR)exception->ExceptionRecord->ExceptionAddress != fault_pc)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (exception->ContextRecord->Pc != fault_pc || exception->ContextRecord->X[16] != 0x1234)
    {
        ExitProcess(2);
    }
    ++handled;
    exception->ContextRecord->Pc += 4;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void mainCRTStartup(void)
{
    PVOID handler = AddVectoredExceptionHandler(1, handle_illegal_instruction);
    if (!handler)
    {
        ExitProcess(1);
    }
    const ULONG64 result = probe_current_el();
    const ULONG removed = RemoveVectoredExceptionHandler(handler);
    ExitProcess(handled == 1 && result == 0x1234 && removed != 0 ? 0 : 3);
}
