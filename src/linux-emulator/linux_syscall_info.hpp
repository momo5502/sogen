#pragma once

#include "std_include.hpp"

#include <arch_emulator.hpp>

namespace sogen
{

    struct linux_syscall_info
    {
        uint64_t number{};
        std::string_view name{};
        std::array<uint64_t, 6> args{};
        x86_64_emulator* emu{};

        uint64_t arg(const size_t index) const
        {
            return this->args.at(index);
        }

        std::optional<std::string> read_c_string(const size_t arg_index, const size_t max_len = 4096) const;
    };

    linux_syscall_info make_linux_syscall_info(x86_64_emulator& emu, const uint64_t number, const std::string_view name);

} // namespace sogen
