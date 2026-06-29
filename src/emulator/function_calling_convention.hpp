#pragma once

namespace sogen
{
    enum class function_calling_convention
    {
        x86_cdecl,
        x86_stdcall,
        x64_fastcall,
        x64_syscall,
    };
}
