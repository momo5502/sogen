#pragma once

#include "../std_include.hpp"

namespace sogen
{

    enum class filesystem_mode
    {
        read_only,
        read_write,
    };

    inline constexpr auto default_root_mode = filesystem_mode::read_write;
    inline constexpr auto default_mount_mode = filesystem_mode::read_write;

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

    inline std::string_view to_string(const filesystem_mode mode)
    {
        switch (mode)
        {
        case filesystem_mode::read_only:
            return "ro";
        case filesystem_mode::read_write:
            return "rw";
        }

        throw std::runtime_error("Unknown filesystem mode");
    }

} // namespace sogen
