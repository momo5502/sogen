#pragma once

#include "posix_fd_table.hpp"
#include "posix_file_system.hpp"
#include "posix_path_resolution.hpp"
#include "posix_result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace sogen
{
    // Linux and XNU share the access(2) mode bits.
    constexpr uint64_t POSIX_X_OK = 0x1;
    constexpr uint64_t POSIX_W_OK = 0x2;

    posix_errno posix_errno_of(posix_path_error error);

    std::variant<std::string, posix_errno> posix_resolve_path_at(const posix_fd_table& fds, std::string_view working_directory,
                                                                 std::optional<int> directory_fd, std::string_view requested_path);

    posix_result posix_chdir(const posix_file_system& file_system, std::string& working_directory, std::string guest_path);
    posix_result posix_fchdir(const posix_fd_table& fds, int fd, std::string& working_directory);
    posix_result posix_access(const posix_file_system& file_system, const std::string& guest_path, uint64_t mode);
}
