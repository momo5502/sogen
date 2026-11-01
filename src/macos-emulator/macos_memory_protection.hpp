#pragma once

#include <memory_permission.hpp>

#include <cstdint>

namespace sogen
{
    // Mach-O segment protections, VM_PROT_* and the BSD PROT_* flags share the same bit values.
    constexpr uint64_t MACOS_PROT_READ = 0x1;
    constexpr uint64_t MACOS_PROT_WRITE = 0x2;
    constexpr uint64_t MACOS_PROT_EXECUTE = 0x4;

    inline memory_permission to_memory_permission(const uint64_t protection)
    {
        auto permission = memory_permission::none;

        if (protection & MACOS_PROT_READ)
        {
            permission |= memory_permission::read;
        }

        if (protection & MACOS_PROT_WRITE)
        {
            permission |= memory_permission::write;
        }

        if (protection & MACOS_PROT_EXECUTE)
        {
            permission |= memory_permission::exec;
        }

        return permission;
    }
}
