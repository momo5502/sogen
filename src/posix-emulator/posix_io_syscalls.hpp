#pragma once

#include "posix_fd_table.hpp"
#include "posix_memory_manager.hpp"
#include "posix_result.hpp"

#include <cstdint>
#include <functional>
#include <string_view>

namespace sogen
{
    using posix_output_callback = std::function<void(std::string_view data)>;

    struct posix_io_context
    {
        posix_fd_table& fds;
        posix_memory_manager& memory;
        const posix_output_callback& on_stdout;
        const posix_output_callback& on_stderr;

        // Linux and XNU number O_NONBLOCK differently, and descriptors keep the guest's open flags.
        int nonblock_flag{};
    };

    posix_result posix_read(const posix_io_context& context, int fd, uint64_t buffer, uint64_t size);
    posix_result posix_write(const posix_io_context& context, int fd, uint64_t buffer, uint64_t size);
}
