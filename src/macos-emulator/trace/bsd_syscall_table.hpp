#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sogen
{
    struct bsd_syscall_argument
    {
        std::string_view type{};
        std::string_view name{};
    };

    struct bsd_syscall_prototype
    {
        std::string_view name{};
        std::string_view return_type{};
        std::span<const bsd_syscall_argument> arguments{};
    };

    extern const std::string_view BSD_SYSCALL_TABLE_XNU_VERSION;
    extern const std::string_view BSD_SYSCALL_TABLE_SOURCE_SHA256;

    size_t bsd_syscall_table_size();
    const bsd_syscall_prototype* find_bsd_syscall_prototype(uint32_t number);
}
