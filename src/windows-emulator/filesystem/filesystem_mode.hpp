#pragma once

#include "../std_include.hpp"

namespace sogen
{

    enum class filesystem_mode
    {
        read_only,
        read_write,
        copy_on_write,
    };

    inline constexpr auto default_root_mode = filesystem_mode::read_only;
    inline constexpr auto default_mount_mode = filesystem_mode::read_only;

    inline std::optional<filesystem_mode> parse_filesystem_mode(const std::string_view name)
    {
        if (name == "ro")
        {
            return filesystem_mode::read_only;
        }

        if (name == "rw")
        {
            return filesystem_mode::read_write;
        }

        return std::nullopt;
    }

    constexpr bool allows_writes(const filesystem_mode mode)
    {
        return mode != filesystem_mode::read_only;
    }

    inline std::string_view to_string(const filesystem_mode mode)
    {
        switch (mode)
        {
        case filesystem_mode::read_only:
            return "ro";
        case filesystem_mode::read_write:
            return "rw";
        case filesystem_mode::copy_on_write:
            return "cow";
        }

        throw std::runtime_error("Unknown filesystem mode");
    }

} // namespace sogen
