#include "posix_ioctl.hpp"
#include "posix_file_system.hpp"
#include "posix_pipe.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <system_error>

namespace sogen
{
    namespace
    {
        std::optional<uint64_t> readable_file_bytes(const posix_fd& file)
        {
            std::error_code ec{};
            const uint64_t size = std::filesystem::file_size(path_from_utf8(file.host_path), ec);
            const auto position = file.seek(0, SEEK_CUR);
            if (ec || !position)
            {
                return std::nullopt;
            }

            return size - std::min(size, *position);
        }

        // A pipe's data is queued on its read end, so the write end and the standard streams, which the guests
        // present as pipes, have nothing to read.
        std::optional<uint64_t> queued_bytes(const posix_fd& file)
        {
            if (file.type == fd_type::pipe_read)
            {
                return posix_pipe_queued_bytes(file);
            }

            if (file.type == fd_type::pipe_write || file.is_standard_stream())
            {
                return 0;
            }

            return readable_file_bytes(file);
        }

        posix_result set_nonblocking(const posix_io_context& context, posix_fd& file, const uint64_t argument)
        {
            int32_t nonblocking{};
            if (!context.memory.is_readable_range(argument, sizeof(nonblocking)) ||
                !context.memory.try_read_memory(argument, &nonblocking, sizeof(nonblocking)))
            {
                return posix_errno::efault;
            }

            posix_set_nonblocking(file, context.nonblock_flag, nonblocking != 0);
            return uint64_t{0};
        }

        posix_result read_queued_bytes(const posix_io_context& context, const posix_fd& file, const uint64_t argument)
        {
            const auto available = queued_bytes(file);
            if (!available)
            {
                return posix_errno::eio;
            }

            const auto reported = static_cast<int32_t>(std::min<uint64_t>(*available, std::numeric_limits<int32_t>::max()));
            if (!context.memory.is_writable_range(argument, sizeof(reported)) ||
                !context.memory.try_write_memory(argument, &reported, sizeof(reported)))
            {
                return posix_errno::efault;
            }

            return uint64_t{0};
        }
    }

    bool posix_has_file_ioctls(const posix_fd& file)
    {
        return file.type == fd_type::file || file.type == fd_type::pipe_read || file.type == fd_type::pipe_write;
    }

    posix_result posix_ioctl(const posix_io_context& context, const int fd, const posix_file_ioctl request, const uint64_t argument)
    {
        auto* file = context.fds.get(fd);
        if (!file)
        {
            return posix_errno::ebadf;
        }

        switch (request)
        {
        case posix_file_ioctl::set_close_on_exec:
        case posix_file_ioctl::clear_close_on_exec:
            context.fds.set_close_on_exec(fd, request == posix_file_ioctl::set_close_on_exec);
            break;
        case posix_file_ioctl::set_nonblocking:
            return set_nonblocking(context, *file, argument);
        case posix_file_ioctl::read_queued_bytes:
            if (!posix_has_file_ioctls(*file))
            {
                return posix_errno::enotty;
            }

            return read_queued_bytes(context, *file, argument);
        case posix_file_ioctl::set_async:
            break;
        }

        return uint64_t{0};
    }
}
