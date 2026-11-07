#include "posix_io_syscalls.hpp"
#include "posix_host_error.hpp"
#include "posix_pipe.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#if defined(_WIN32)
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
#if defined(_WIN32)
            // Anonymous pipes on Windows cannot be made non-blocking, so an empty pipe has to be detected before reading.
            if (file.type == fd_type::pipe_read && (file.flags & nonblock_flag) != 0 && posix_pipe_queued_bytes(file) == 0)
            {
                return posix_errno::eagain;
            }
#else
            (void)nonblock_flag;
#endif

            errno = 0;
            const auto bytes_read = read_host_descriptor(host_descriptor(file.handle), chunk);
            if (bytes_read < 0)
            {
                return posix_errno_from_host(errno);
            }

            return static_cast<uint64_t>(bytes_read);
        }

        // A host error after some bytes moved is reported by the next call, as a kernel reports a short transfer first.
        posix_result host_file_transfer_result(FILE* handle, const size_t transferred)
        {
            if (!std::ferror(handle))
            {
                return uint64_t{transferred};
            }

            const auto host_errno = errno;
            std::clearerr(handle);
            if (transferred > 0)
            {
                return uint64_t{transferred};
            }

            return posix_errno_from_host(host_errno);
        }

        posix_result read_host_file(FILE* handle, const std::span<std::byte> chunk)
        {
            errno = 0;
            const auto bytes_read = std::fread(chunk.data(), 1, chunk.size(), handle);
            return host_file_transfer_result(handle, bytes_read);
        }

        posix_result read_seekable(posix_fd& file, const std::span<std::byte> chunk)
        {
            if (file.type == fd_type::memory_file)
            {
                return uint64_t{read_memory_file(*file.memory_file, chunk)};
            }

            return read_host_file(file.handle, chunk);
        }

        void unread_seekable(const posix_fd& file, const size_t count)
        {
            if (file.type == fd_type::memory_file)
            {
                file.memory_file->offset -= count;
                return;
            }

            file.seek(-static_cast<int64_t>(count), SEEK_CUR);
        }

        posix_result copy_to_guest(const posix_io_context& context, const uint64_t destination, const std::span<const std::byte> data)
        {
            if (!data.empty() && !context.memory.try_write_memory(destination, data.data(), data.size()))
            {
                return posix_errno::efault;
            }

            return uint64_t{data.size()};
        }

        // A stream cannot take back what it delivered, so it is only asked for what the guest buffer can receive.
        posix_result read_stream_chunk(const posix_io_context& context, const posix_fd& file, const uint64_t destination,
                                       const std::span<std::byte> chunk)
        {
            const auto receivable = chunk.first(context.memory.writable_prefix_size(destination, chunk.size()));
            if (receivable.empty())
            {
                return posix_errno::efault;
            }

            const auto result = read_host_stream(file, receivable, context.nonblock_flag);
            if (std::holds_alternative<posix_errno>(result))
            {
                return result;
            }

            return copy_to_guest(context, destination, receivable.first(static_cast<size_t>(std::get<uint64_t>(result))));
        }

        // As a kernel does, a file is read first and whatever does not fit the guest buffer is left unread.
        posix_result read_seekable_chunk(const posix_io_context& context, posix_fd& file, const uint64_t destination,
                                         const std::span<std::byte> chunk)
        {
            const auto result = read_seekable(file, chunk);
            if (std::holds_alternative<posix_errno>(result))
            {
                return result;
            }

            const auto bytes_read = static_cast<size_t>(std::get<uint64_t>(result));
            const auto copied = context.memory.writable_prefix_size(destination, bytes_read);
            if (copied < bytes_read)
            {
                unread_seekable(file, bytes_read - copied);
            }

            if (copied == 0 && bytes_read > 0)
            {
                return posix_errno::efault;
            }

            return copy_to_guest(context, destination, chunk.first(copied));
        }

        // A FIFO or device opened by path cannot rewind what it delivered either, so it is read like a stream.
        bool reads_like_stream(const posix_fd& file)
        {
            return is_host_stream(file) || (file.type == fd_type::file && !file.seek(0, SEEK_CUR));
        }

        posix_result read_chunk(const posix_io_context& context, posix_fd& file, const bool is_stream, const uint64_t destination,
                                const std::span<std::byte> chunk)
        {
            return is_stream ? read_stream_chunk(context, file, destination, chunk)
                             : read_seekable_chunk(context, file, destination, chunk);
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
            errno = 0;
            const auto written = std::fwrite(chunk.data(), 1, chunk.size(), handle);
            return host_file_transfer_result(handle, written);
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

        const auto is_stream = reads_like_stream(*file);
        std::vector<std::byte> chunk(static_cast<size_t>(std::min<uint64_t>(size, TRANSFER_CHUNK_SIZE)));
        uint64_t transferred = 0;
        while (transferred < size)
        {
            const auto chunk_size = static_cast<size_t>(std::min<uint64_t>(size - transferred, chunk.size()));
            const auto result = read_chunk(context, *file, is_stream, buffer + transferred, std::span{chunk}.first(chunk_size));
            if (const auto* error = std::get_if<posix_errno>(&result))
            {
                return posix_partial_transfer(transferred, *error);
            }

            const auto bytes_read = std::get<uint64_t>(result);
            transferred += bytes_read;
            if (bytes_read < chunk_size || is_stream)
            {
                break;
            }
        }

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
            const auto source = buffer + transferred;
            const auto chunk_size = static_cast<size_t>(std::min<uint64_t>(size - transferred, chunk.size()));
            if (!context.memory.is_readable_range(source, chunk_size) || !context.memory.try_read_memory(source, chunk.data(), chunk_size))
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
