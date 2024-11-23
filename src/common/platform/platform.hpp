#pragma once

#ifdef _WIN32
    #pragma warning(disable: 4201) // nameless struct/union
    #pragma warning(disable: 4702) // unreachable code
#endif

#include "compiler.hpp"
#include "primitives.hpp"
#include "traits.hpp"
#include "unicode.hpp"
#include "status.hpp"
#include "process.hpp"
#include "kernel_mapped.hpp"
#include "memory.hpp"
#include "file_management.hpp"
#include "win_pefile.hpp"
#include "synchronisation.hpp"
#include "registry.hpp"
#include "network.hpp"
#include "threading.hpp"

#ifdef OS_WINDOWS
    #pragma comment(lib, "ntdll")
    
extern "C"
{
    NTSYSCALLAPI
    NTSTATUS
    NTAPI
    NtQuerySystemInformationEx(
        _In_ SYSTEM_INFORMATION_CLASS SystemInformationClass,
        _In_reads_bytes_(InputBufferLength) PVOID InputBuffer,
        _In_ ULONG InputBufferLength,
        _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
        _In_ ULONG SystemInformationLength,
        _Out_opt_ PULONG ReturnLength
        );
}
#endif