#pragma once

#include "posix_fd_table.hpp"
#include "posix_file_system.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace sogen
{
    enum class posix_path_error
    {
        bad_descriptor,
        not_a_directory,
    };

    // A missing directory descriptor stands for AT_FDCWD, whose value differs between guest operating systems.
    inline std::optional<std::string> posix_resolve_guest_path_at(const posix_fd_table& fds, const std::string_view working_directory,
                                                                  const std::optional<int> directory_fd, const std::string_view guest_path,
                                                                  posix_path_error& error)
    {
        error = posix_path_error::bad_descriptor;

        if (guest_path.starts_with('/') || guest_path.starts_with('\\'))
        {
            return posix_file_system::normalize_guest_path_string(guest_path);
        }

        if (!directory_fd)
        {
            return posix_file_system::resolve_guest_path_string(working_directory, guest_path);
        }

        const auto* directory = fds.get(*directory_fd);
        if (!directory)
        {
            return std::nullopt;
        }

        if (directory->type != fd_type::directory)
        {
            error = posix_path_error::not_a_directory;
            return std::nullopt;
        }

        if (directory->guest_path.empty())
        {
            return std::nullopt;
        }

        return posix_file_system::resolve_guest_path_string(directory->guest_path, guest_path);
    }
}
