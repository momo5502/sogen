#include <windows.h>
#include <xmmintrin.h>

_Static_assert(sizeof(void*) == 4, "Build this probe for x86");

static DWORD tls_slot;
static volatile LONG handled_exceptions;

static void emit(const char* text)
{
    DWORD length = 0;
    DWORD written = 0;
    while (text[length])
    {
        ++length;
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}

static void require(BOOL condition, const char* failure)
{
    if (!condition)
    {
        emit(failure);
        ExitProcess(71);
    }
}

static BOOL contains(const char* text, const char* needle)
{
    for (; *text; ++text)
    {
        DWORD i = 0;
        while (needle[i] && text[i] == needle[i])
        {
            ++i;
        }
        if (!needle[i])
        {
            return TRUE;
        }
    }
    return FALSE;
}

static DWORD WINAPI worker(void* unused)
{
    (void)unused;
    if (TlsGetValue(tls_slot) != NULL || !TlsSetValue(tls_slot, (void*)0x5678) || TlsGetValue(tls_slot) != (void*)0x5678)
    {
        return 72;
    }
    return 35;
}

static LONG WINAPI exception_handler(EXCEPTION_POINTERS* exception)
{
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    if (code == 0xe1234567)
    {
        ++handled_exceptions;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION)
    {
        exception->ContextRecord->Eip += 2;
        ++handled_exceptions;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void)
{
    const char* arguments = GetCommandLineA();
    require(contains(arguments, "wow64-argument"), "FAIL arguments\n");
    emit("POINTER_BYTES=4 ARGS=OK\n");

    DWORD* memory = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    require(memory != NULL, "FAIL allocation\n");
    memory[0] = 0x12345678;
    memory[0x1000] = memory[0] + 1;
    require(memory[0x1000] == 0x12345679, "FAIL memory\n");
    require(VirtualFree(memory, 0, MEM_RELEASE), "FAIL free\n");
    emit("MEMORY=OK\n");

    volatile float left = 6.0f;
    volatile float right = 7.0f;
    const float product = _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(left), _mm_set_ss(right)));
    require(product == 42.0f, "FAIL SSE\n");
    emit("SSE=OK\n");

    tls_slot = TlsAlloc();
    require(tls_slot != TLS_OUT_OF_INDEXES && TlsSetValue(tls_slot, (void*)0x1234), "FAIL TLS allocation\n");
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    require(thread != NULL, "FAIL thread creation\n");
    require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "FAIL thread wait\n");
    DWORD thread_exit;
    require(GetExitCodeThread(thread, &thread_exit) && thread_exit == 35, "FAIL thread exit\n");
    require(TlsGetValue(tls_slot) == (void*)0x1234, "FAIL TLS isolation\n");
    CloseHandle(thread);
    TlsFree(tls_slot);
    emit("THREAD_TLS=OK\n");

    void* handler = AddVectoredExceptionHandler(1, exception_handler);
    require(handler != NULL, "FAIL exception handler\n");
    RaiseException(0xe1234567, 0, 0, NULL);

    union
    {
        BYTE* bytes;
        void(WINAPI* execute)(void);
    } instruction;

    instruction.bytes = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    require(instruction.bytes != NULL, "FAIL instruction allocation\n");
    instruction.bytes[0] = 0x0f;
    instruction.bytes[1] = 0x0b;
    instruction.bytes[2] = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), instruction.bytes, 3);
    instruction.execute();
    VirtualFree(instruction.bytes, 0, MEM_RELEASE);
    require(handled_exceptions == 2, "FAIL exception dispatch\n");
    RemoveVectoredExceptionHandler(handler);
    emit("SOFTWARE_HARDWARE_EXCEPTIONS=OK\nWOW64_PROBE_PASS\n");
    return contains(arguments, "--exit-37") ? 37 : 0;
}
