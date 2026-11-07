#pragma once

#include <memory_permission.hpp>
#include <posix_memory_syscalls.hpp>

#include <cstdint>

namespace sogen
{
    // Mach-O segment protections, VM_PROT_* and the BSD PROT_* flags share the same bit values.
    constexpr uint64_t MACOS_PROT_READ = POSIX_PROT_READ;
    constexpr uint64_t MACOS_PROT_WRITE = POSIX_PROT_WRITE;
    constexpr uint64_t MACOS_PROT_EXECUTE = POSIX_PROT_EXEC;

    inline memory_permission to_memory_permission(const uint64_t protection)
    {
        return posix_memory_permission(protection);
    }
}
