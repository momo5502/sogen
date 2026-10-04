#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sogen
{
    struct mach_trap_prototype
    {
        std::string_view name{};
        uint32_t argument_count{};
    };

    extern const std::string_view MACH_TRAP_TABLE_XNU_VERSION;

    size_t mach_trap_table_size();
    const mach_trap_prototype* find_mach_trap_prototype(uint32_t index);
}
