#include "posix_io_syscalls.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sogen
{
    namespace
    {
        // Linux and XNU share O_ACCMODE and its values.
        constexpr int ACCESS_MODE_MASK = 0x3;
        constexpr int ACCESS_READ_ONLY = 0x0;
        constexpr int ACCESS_WRITE_ONLY = 0x1;

        constexpr size_t TRANSFER_CHUNK_SIZE = 0x10000;

        posix_errno posix_errno_from_host(const int host_errno)
        {
            switch (host_errno)
            {
            case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
                return posix_errno::eagain;
            case EINTR:
                return posix_errno::eintr;
            case EBADF:
                return posix_errno::ebadf;
            case EINVAL:
                return posix_errno::einval;
            case EPIPE:
                return posix_errno::epipe;
            case ENOSPC:
                return posix_errno::enospc;
            default:
                return posix_errno::eio;
            }
        }

        int host_descriptor(FILE* handle)
        {
#if defined(_WIN32)
            return _fileno(handle);
#else
            return ::fileno(handle);
#endif
        }

        int64_t read_host_descriptor(const int descriptor, const std::span<std::byte> buffer)
        {
#if defined(_WIN32)
            const auto count = static_cast<unsigned int>(std::min<size_t>(buffer.size(), std::numeric_limits<unsigned int>::max()));
            return _read(descriptor, buffer.data(), count);
#else
            return ::read(descriptor, buffer.data(), buffer.size());
#endif
        }

        int64_t write_host_descriptor(const int descriptor, const std::span<const std::byte> buffer)
        {
#if defined(_WIN32)
            const auto count = static_cast<unsigned int>(std::min<size_t>(buffer.size(), std::numeric_limits<unsigned int>::max()));
            return _write(descriptor, buffer.data(), count);
#else
            return ::write(descriptor, buffer.data(), buffer.size());
#endif
        }

#if defined(_WIN32)
        // Anonymous pipes on Windows cannot be made non-blocking, so an empty pipe has to be detected before reading.
        bool is_empty_host_pipe(const int descriptor)
        {
            auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
            DWORD available = 0;
            return handle != INVALID_HANDLE_VALUE && PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) != 0 && available == 0;
        }
#endif

        bool is_output_stream(const posix_fd& file)
        {
            return file.handle == stdout || file.handle == stderr;
        }

        // Pipes and the standard streams hand over what they have, so a read must not wait for the whole buffer.
        bool is_host_stream(const posix_fd& file)
        {
            return file.type == fd_type::pipe_read || file.type == fd_type::pipe_write || file.is_standard_stream();
        }

        int access_mode(const posix_fd& file)
        {
            return file.flags & ACCESS_MODE_MASK;
        }

        std::optional<posix_errno> read_error(const posix_fd& file)
        {
            switch (file.type)
            {
            case fd_type::file:
                return file.handle != nullptr && access_mode(file) != ACCESS_WRITE_ONLY ? std::nullopt : std::optional{posix_errno::ebadf};
            case fd_type::memory_file:
                return file.memory_file ? std::nullopt : std::optional{posix_errno::ebadf};
            case fd_type::pipe_read:
                return std::nullopt;
            case fd_type::directory:
                return posix_errno::eisdir;
            case fd_type::socket:
                return posix_errno::enotconn;
            default:
                return posix_errno::ebadf;
            }
        }

        std::optional<posix_errno> write_error(const posix_fd& file)
        {
            switch (file.type)
            {
            case fd_type::file:
                return file.handle != nullptr && access_mode(file) != ACCESS_READ_ONLY ? std::nullopt : std::optional{posix_errno::ebadf};
            case fd_type::pipe_write:
                return std::nullopt;
            case fd_type::socket:
                return posix_errno::enotconn;
            default:
                return posix_errno::ebadf;
            }
        }

        size_t read_memory_file(posix_memory_fd& memory_file, const std::span<std::byte> chunk)
        {
            const auto content = std::as_bytes(std::span{memory_file.content});
            const auto available = content.subspan(std::min(memory_file.offset, content.size()));
            const auto count = std::min(available.size(), chunk.size());
            std::ranges::copy(available.first(count), chunk.begin());
            memory_file.offset += count;
            return count;
        }

        posix_result read_host_stream(const posix_fd& file, const std::span<std::byte> chunk, const int nonblock_flag)
        {
            const auto descriptor = host_descriptor(file.handle);
#if defined(_WIN32)
            if (file.type == fd_type::pipe_read && (file.flags & nonblock_flag) != 0 && is_empty_host_pipe(descriptor))
            {
                return posix_errno::eagain;
            }
#else
            (void)nonblock_flag;
#endif

            errno = 0;
            const auto bytes_read = read_host_descriptor(descriptor, chunk);
            if (bytes_read < 0)
            {
                return posix_errno_from_host(errno);
            }

            return static_cast<uint64_t>(bytes_read);
        }

        posix_result read_host_file(FILE* handle, const std::span<std::byte> chunk)
        {
            const auto bytes_read = std::fread(chunk.data(), 1, chunk.size(), handle);
            if (bytes_read < chunk.size() && std::ferror(handle))
            {
                std::clearerr(handle);
                return posix_errno::eio;
            }

            return uint64_t{bytes_read};
        }

        posix_result read_chunk(posix_fd& file, const std::span<std::byte> chunk, const int nonblock_flag)
        {
            if (file.type == fd_type::memory_file)
            {
                return uint64_t{read_memory_file(*file.memory_file, chunk)};
            }

            if (is_host_stream(file))
            {
                return read_host_stream(file, chunk, nonblock_flag);
            }

            return read_host_file(file.handle, chunk);
        }

        posix_result write_output_stream(const posix_io_context& context, FILE* stream, const std::span<const std::byte> chunk)
        {
            const auto& callback = stream == stdout ? context.on_stdout : context.on_stderr;
            if (callback)
            {
                callback(std::string_view{reinterpret_cast<const char*>(chunk.data()), chunk.size()});
                return uint64_t{chunk.size()};
            }

            const auto written = std::fwrite(chunk.data(), 1, chunk.size(), stream);
            std::fflush(stream);
            return uint64_t{written};
        }

        posix_result write_host_stream(FILE* handle, const std::span<const std::byte> chunk)
        {
            errno = 0;
            const auto written = write_host_descriptor(host_descriptor(handle), chunk);
            if (written < 0)
            {
                return posix_errno_from_host(errno);
            }

            return static_cast<uint64_t>(written);
        }

        posix_result write_host_file(FILE* handle, const std::span<const std::byte> chunk)
        {
            const auto written = std::fwrite(chunk.data(), 1, chunk.size(), handle);
            if (written < chunk.size() && std::ferror(handle))
            {
                std::clearerr(handle);
                return posix_errno::eio;
            }

            return uint64_t{written};
        }

        posix_result write_chunk(const posix_io_context& context, const posix_fd& file, const std::span<const std::byte> chunk)
        {
            if (is_output_stream(file))
            {
                return write_output_stream(context, file.handle, chunk);
            }

            if (is_host_stream(file))
            {
                return write_host_stream(file.handle, chunk);
            }

            return write_host_file(file.handle, chunk);
        }
    }

    posix_result posix_read(const posix_io_context& context, const int fd, const uint64_t buffer, const uint64_t size)
    {
        auto* file = context.fds.get(fd);
        if (file == nullptr)
        {
            return posix_errno::ebadf;
        }

        if (const auto error = read_error(*file))
        {
            return *error;
        }

        std::vector<std::byte> chunk(static_cast<size_t>(std::min<uint64_t>(size, TRANSFER_CHUNK_SIZE)));
        uint64_t transferred = 0;
        do
        {
            const auto destination = buffer + transferred;
            const auto chunk_size = static_cast<size_t>(std::min<uint64_t>(size - transferred, chunk.size()));

            // A stream cannot give back what it delivered, so the destination must be writable before anything is
            // taken from the host; XNU only consumes what it copies out.
            if (!context.memory.is_writable_range(destination, chunk_size))
            {
                return posix_partial_transfer(transferred, posix_errno::efault);
            }

            const auto result = read_chunk(*file, std::span{chunk}.first(chunk_size), context.nonblock_flag);
            if (const auto* error = std::get_if<posix_errno>(&result))
            {
                return posix_partial_transfer(transferred, *error);
            }

            const auto bytes_read = static_cast<size_t>(std::get<uint64_t>(result));
            if (!context.memory.try_write_memory(destination, chunk.data(), bytes_read))
            {
                return posix_partial_transfer(transferred, posix_errno::efault);
            }

            transferred += bytes_read;
            if (bytes_read < chunk_size || is_host_stream(*file))
            {
                break;
            }
        } while (transferred < size);

        return transferred;
    }

    posix_result posix_write(const posix_io_context& context, const int fd, const uint64_t buffer, const uint64_t size)
    {
        const auto* file = context.fds.get(fd);
        if (file == nullptr)
        {
            return posix_errno::ebadf;
        }

        if (const auto error = write_error(*file))
        {
            return *error;
        }

        std::vector<std::byte> chunk(static_cast<size_t>(std::min<uint64_t>(size, TRANSFER_CHUNK_SIZE)));
        uint64_t transferred = 0;
        while (transferred < size)
        {
            const auto chunk_size = static_cast<size_t>(std::min<uint64_t>(size - transferred, chunk.size()));
            if (!context.memory.try_read_memory(buffer + transferred, chunk.data(), chunk_size))
            {
                return posix_partial_transfer(transferred, posix_errno::efault);
            }

            const auto result = write_chunk(context, *file, std::span{chunk}.first(chunk_size));
            if (const auto* error = std::get_if<posix_errno>(&result))
            {
                return posix_partial_transfer(transferred, *error);
            }

            const auto written = std::get<uint64_t>(result);
            transferred += written;
            if (written < chunk_size)
            {
                break;
            }
        }

        return transferred;
    }
}
