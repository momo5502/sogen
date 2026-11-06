#pragma once

#include "posix_fd_table.hpp"
#include "posix_result.hpp"

#include <cstdint>

namespace sogen
{
    // Linux and XNU both define FD_CLOEXEC as 1.
    constexpr uint64_t POSIX_FD_CLOEXEC = 1;

    posix_result posix_close(posix_fd_table& fds, int fd);
    posix_result posix_dup(posix_fd_table& fds, int fd, int64_t minimum_fd, bool close_on_exec, int descriptor_limit);
    posix_result posix_dup2(posix_fd_table& fds, int fd, int new_fd, int descriptor_limit);

    posix_result posix_get_descriptor_flags(const posix_fd_table& fds, int fd);
    posix_result posix_set_descriptor_flags(posix_fd_table& fds, int fd, uint64_t flags);
    posix_result posix_get_status_flags(const posix_fd_table& fds, int fd);

    posix_result posix_lseek(posix_fd_table& fds, int fd, int64_t offset, uint64_t whence);
}
