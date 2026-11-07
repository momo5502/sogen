#pragma once

#include "posix_io_syscalls.hpp"
#include "posix_result.hpp"

#include <cstdint>

namespace sogen
{
    // The FIO* requests Linux and XNU both define, under different numbers.
    enum class posix_file_ioctl
    {
        set_close_on_exec,
        clear_close_on_exec,
        set_nonblocking,
        set_async,
        read_queued_bytes,
    };

    // Files, pipes and the standard streams; other descriptors have their own ioctls.
    bool posix_has_file_ioctls(const posix_fd& file);

    posix_result posix_ioctl(const posix_io_context& context, int fd, posix_file_ioctl request, uint64_t argument);
}
