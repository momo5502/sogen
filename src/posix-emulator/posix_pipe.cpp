#include "posix_pipe.hpp"

#include <array>
#include <cstdio>
#include <fcntl.h>
#include <utility>

#if defined(_WIN32)
#include <utils/win.hpp>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace sogen
{
    namespace
    {
        // Linux and XNU share O_RDONLY and O_WRONLY.
        constexpr int ACCESS_READ_ONLY = 0x0;
        constexpr int ACCESS_WRITE_ONLY = 0x1;

        bool create_host_pipe(std::array<int, 2>& descriptors)
        {
#if defined(_WIN32)
            return _pipe(descriptors.data(), 4096, _O_BINARY) == 0;
#else
            return ::pipe(descriptors.data()) == 0;
#endif
        }

        void close_host_descriptor(const int descriptor)
        {
#if defined(_WIN32)
            _close(descriptor);
#else
            ::close(descriptor);
#endif
        }

        int host_descriptor(FILE* handle)
        {
#if defined(_WIN32)
            return _fileno(handle);
#else
            return ::fileno(handle);
#endif
        }

        // Windows anonymous pipes have no such flag, so a non-blocking read is emulated when it happens.
        void set_host_nonblocking(const int descriptor, const bool nonblocking)
        {
#if defined(_WIN32)
            (void)descriptor;
            (void)nonblocking;
#else
            const auto flags = ::fcntl(descriptor, F_GETFL, 0);
            if (flags >= 0)
            {
                ::fcntl(descriptor, F_SETFL, nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
            }
#endif
        }

        void set_host_close_on_exec(const int descriptor)
        {
#if defined(_WIN32)
            (void)descriptor;
#else
            const auto flags = ::fcntl(descriptor, F_GETFD, 0);
            if (flags >= 0)
            {
                ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
            }
#endif
        }

        FILE* open_pipe_stream(const int descriptor, const char* mode)
        {
#if defined(_WIN32)
            auto* stream = _fdopen(descriptor, mode);
#else
            auto* stream = ::fdopen(descriptor, mode);
#endif
            if (stream == nullptr)
            {
                close_host_descriptor(descriptor);
                return nullptr;
            }

            std::setvbuf(stream, nullptr, _IONBF, 0);
            return stream;
        }

        posix_fd make_pipe_end(const fd_type type, FILE* stream, const int flags)
        {
            posix_fd end{};
            end.type = type;
            end.handle = stream;
            end.flags = flags;
            return end;
        }
    }

    std::variant<posix_pipe, posix_errno> posix_create_pipe(posix_fd_table& fds, const int nonblock_flag, const bool close_on_exec)
    {
        std::array<int, 2> descriptors{-1, -1};
        if (!create_host_pipe(descriptors))
        {
            return posix_errno::emfile;
        }

        for (const auto descriptor : descriptors)
        {
            if (nonblock_flag != 0)
            {
                set_host_nonblocking(descriptor, true);
            }

            if (close_on_exec)
            {
                set_host_close_on_exec(descriptor);
            }
        }

        auto read_end = make_pipe_end(fd_type::pipe_read, open_pipe_stream(descriptors[0], "rb"), ACCESS_READ_ONLY | nonblock_flag);
        auto write_end = make_pipe_end(fd_type::pipe_write, open_pipe_stream(descriptors[1], "wb"), ACCESS_WRITE_ONLY | nonblock_flag);
        if (read_end.handle == nullptr || write_end.handle == nullptr)
        {
            return posix_errno::emfile;
        }

        return posix_pipe{
            .read_fd = fds.allocate(std::move(read_end), close_on_exec),
            .write_fd = fds.allocate(std::move(write_end), close_on_exec),
        };
    }

    std::optional<uint64_t> posix_pipe_queued_bytes(const posix_fd& read_end)
    {
        const auto descriptor = host_descriptor(read_end.handle);
#if defined(_WIN32)
        auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
        DWORD available = 0;
        if (handle == INVALID_HANDLE_VALUE || PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) == 0)
        {
            return std::nullopt;
        }

        return uint64_t{available};
#else
        int available = 0;
        if (::ioctl(descriptor, FIONREAD, &available) != 0 || available < 0)
        {
            return std::nullopt;
        }

        return static_cast<uint64_t>(available);
#endif
    }

    void posix_set_nonblocking(posix_fd& file, const int nonblock_flag, const bool nonblocking)
    {
        file.flags = nonblocking ? (file.flags | nonblock_flag) : (file.flags & ~nonblock_flag);
        if (file.type == fd_type::pipe_read || file.type == fd_type::pipe_write)
        {
            set_host_nonblocking(host_descriptor(file.handle), nonblocking);
        }
    }
}
