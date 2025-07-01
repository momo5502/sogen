#pragma once
#include <cstdint>
#include <string>
#include <emulator.hpp>

inline std::string get_permission_string(const memory_permission permission)
{
    const bool has_exec = (permission & memory_permission::exec) != memory_permission::none;
    const bool has_read = (permission & memory_permission::read) != memory_permission::none;
    const bool has_write = (permission & memory_permission::write) != memory_permission::none;

    std::string res = {};
    res.reserve(3);

    res.push_back(has_read ? 'r' : '-');
    res.push_back(has_write ? 'w' : '-');
    res.push_back(has_exec ? 'x' : '-');

    return res;
}

inline nt_memory_permission map_nt_to_emulator_protection(uint32_t nt_protection)
{
    memory_permission_ext ext = memory_permission_ext::none;
    // TODO: Check for invalid combinations
    if (nt_protection & PAGE_GUARD)
    {
        // Unset the guard flag so the following switch statement will still work
        nt_protection &= ~static_cast<uint32_t>(PAGE_GUARD);
        ext = memory_permission_ext::guard;
    }

    memory_permission common = memory_permission::none;
    switch (nt_protection)
    {
        case PAGE_NOACCESS:
            common = memory_permission::none;
            break;
        case PAGE_READONLY:
            common = memory_permission::read;
            break;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
            common = memory_permission::read | memory_permission::write;
            break;
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
            common = memory_permission::read | memory_permission::exec;
            break;
        case PAGE_EXECUTE_READWRITE:
            common = memory_permission::all;
            break;
        case PAGE_EXECUTE_WRITECOPY:
        default:
            throw std::runtime_error("Failed to map protection");
    }

    return nt_memory_permission { common, ext };
}

inline uint32_t map_emulator_to_nt_protection(const memory_permission permission)
{
    const bool has_exec = (permission & memory_permission::exec) != memory_permission::none;
    const bool has_read = (permission & memory_permission::read) != memory_permission::none;
    const bool has_write = (permission & memory_permission::write) != memory_permission::none;

    if (!has_read)
    {
        return PAGE_NOACCESS;
    }

    if (has_exec && has_write)
    {
        return PAGE_EXECUTE_READWRITE;
    }

    if (has_exec)
    {
        return PAGE_EXECUTE_READ;
    }

    if (has_write)
    {
        return PAGE_READWRITE;
    }

    return PAGE_READONLY;
}
