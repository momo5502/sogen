#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace sogen
{
    struct macos_flag_bit
    {
        uint64_t mask{};
        std::string_view name{};
    };

    std::string format_flag_bits(uint64_t value, std::span<const macos_flag_bit> bits);
    std::string format_open_flags(uint64_t value);
    std::string format_mmap_protection(uint64_t value);
    std::string format_mmap_flags(uint64_t value);
    std::string format_madvise_advice(uint64_t value);
    std::string format_fcntl_command(uint64_t value);
    std::string format_seek_whence(uint64_t value);
    std::string format_file_mode(uint64_t value);

    // Empty for a value with no name, never a placeholder: the caller decides how an unknown renders, and
    // the trace prints the number it already has rather than inventing a label for it.
    std::string_view macos_errno_name(int64_t error);
}
