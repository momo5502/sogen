#pragma once

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-use-enum-class)

#define DEBUG_KILL_ON_CLOSE 0x1

#ifndef OS_WINDOWS
#define DBG_CONTINUE              0x00010002
#define DBG_EXCEPTION_NOT_HANDLED 0x80010001
#endif

namespace sogen
{
    // Layouts from phnt's ntdbg.h, as ntdll's DbgUiConvertStateChangeStructure reads them.
    enum DBG_STATE
    {
        DbgIdle,
        DbgReplyPending,
        DbgCreateThreadStateChange,
        DbgCreateProcessStateChange,
        DbgExitThreadStateChange,
        DbgExitProcessStateChange,
        DbgExceptionStateChange,
        DbgBreakpointStateChange,
        DbgSingleStepStateChange,
        DbgLoadDllStateChange,
        DbgUnloadDllStateChange,
    };

    struct DBGKM_EXCEPTION64
    {
        EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>> ExceptionRecord;
        ULONG FirstChance;
    };

    struct DBGKM_CREATE_THREAD64
    {
        ULONG SubSystemKey;
        EmulatorTraits<Emu64>::PVOID StartAddress;
    };

    struct DBGKM_CREATE_PROCESS64
    {
        ULONG SubSystemKey;
        EmulatorTraits<Emu64>::HANDLE FileHandle;
        EmulatorTraits<Emu64>::PVOID BaseOfImage;
        ULONG DebugInfoFileOffset;
        ULONG DebugInfoSize;
        DBGKM_CREATE_THREAD64 InitialThread;
    };

    struct DBGKM_EXIT64
    {
        NTSTATUS ExitStatus;
    };

    struct DBGKM_LOAD_DLL64
    {
        EmulatorTraits<Emu64>::HANDLE FileHandle;
        EmulatorTraits<Emu64>::PVOID BaseOfDll;
        ULONG DebugInfoFileOffset;
        ULONG DebugInfoSize;
        EmulatorTraits<Emu64>::PVOID NamePointer;
    };

    struct DBGKM_UNLOAD_DLL64
    {
        EmulatorTraits<Emu64>::PVOID BaseAddress;
    };

    struct DBGUI_CREATE_THREAD64
    {
        EmulatorTraits<Emu64>::HANDLE HandleToThread;
        DBGKM_CREATE_THREAD64 NewThread;
    };

    struct DBGUI_CREATE_PROCESS64
    {
        EmulatorTraits<Emu64>::HANDLE HandleToProcess;
        EmulatorTraits<Emu64>::HANDLE HandleToThread;
        DBGKM_CREATE_PROCESS64 NewProcess;
    };

    struct DBGUI_WAIT_STATE_CHANGE64
    {
        DBG_STATE NewState;
        CLIENT_ID64 AppClientId;

        union
        {
            DBGKM_EXCEPTION64 Exception;
            DBGUI_CREATE_THREAD64 CreateThread;
            DBGUI_CREATE_PROCESS64 CreateProcessInfo;
            DBGKM_EXIT64 ExitThread;
            DBGKM_EXIT64 ExitProcess;
            DBGKM_LOAD_DLL64 LoadDll;
            DBGKM_UNLOAD_DLL64 UnloadDll;
        } StateInfo;
    };

    static_assert(sizeof(DBGKM_EXCEPTION64) == 0xA0);
    static_assert(sizeof(DBGKM_CREATE_PROCESS64) == 0x30);
    static_assert(sizeof(DBGKM_LOAD_DLL64) == 0x20);
    static_assert(offsetof(DBGUI_WAIT_STATE_CHANGE64, AppClientId) == 0x8);
    static_assert(offsetof(DBGUI_WAIT_STATE_CHANGE64, StateInfo) == 0x18);
    static_assert(sizeof(DBGUI_WAIT_STATE_CHANGE64) == 0xB8);
}

// NOLINTEND(modernize-use-using,cppcoreguidelines-use-enum-class)
