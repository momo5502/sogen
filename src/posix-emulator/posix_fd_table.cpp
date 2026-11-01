#include "posix_fd_table.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sogen
{
    namespace
    {
        bool is_stdio_path(const std::string& path)
        {
            return path == "/dev/stdin" || path == "/dev/stdout" || path == "/dev/stderr";
        }

        FILE* stdio_handle_for_path(const std::string& path)
        {
            if (path == "/dev/stdin")
            {
                return stdin;
            }
            if (path == "/dev/stdout")
            {
                return stdout;
            }
            if (path == "/dev/stderr")
            {
                return stderr;
            }
            return nullptr;
        }

        int open_flags_for_snapshot(const int flags)
        {
            const auto access = flags & 3;
#if defined(_WIN32)
            auto open_flags = _O_BINARY;
            if (access == 0)
            {
                open_flags |= _O_RDONLY;
            }
            else if (access == 1)
            {
                open_flags |= _O_WRONLY;
            }
            else
            {
                open_flags |= _O_RDWR;
            }
            if ((flags & 02000) != 0)
            {
                open_flags |= _O_APPEND;
            }
#else
            auto open_flags = access;
            if ((flags & 02000) != 0)
            {
                open_flags |= O_APPEND;
            }
#endif
            return open_flags;
        }

        const char* fdopen_mode_for_flags(const int flags)
        {
            const auto access = flags & 3;
            if (access == 0)
            {
                return "rb";
            }

            if ((flags & 02000) != 0)
            {
                return access == 2 ? "a+b" : "ab";
            }

            return access == 2 ? "r+b" : "wb";
        }

        int open_snapshot_host_file(const std::string& host_path, const int flags)
        {
#if defined(_WIN32)
            return _open(host_path.c_str(), open_flags_for_snapshot(flags));
#else
            return ::open(host_path.c_str(), open_flags_for_snapshot(flags));
#endif
        }

        FILE* fdopen_snapshot_host_file(const int fd, const int flags)
        {
#if defined(_WIN32)
            return _fdopen(fd, fdopen_mode_for_flags(flags));
#else
            return ::fdopen(fd, fdopen_mode_for_flags(flags));
#endif
        }

        void close_snapshot_host_file(const int fd)
        {
#if defined(_WIN32)
            _close(fd);
#else
            ::close(fd);
#endif
        }

        bool seek_host_file(FILE* handle, const int64_t offset, const int origin = SEEK_SET)
        {
#if defined(_WIN32)
            return _fseeki64(handle, offset, origin) == 0;
#else
            return ::fseeko(handle, static_cast<off_t>(offset), origin) == 0;
#endif
        }

        int64_t read_host_descriptor(FILE* handle, const std::span<std::byte> buffer)
        {
#if defined(_WIN32)
            const auto count = static_cast<unsigned int>(std::min<size_t>(buffer.size(), std::numeric_limits<unsigned int>::max()));
            return _read(_fileno(handle), buffer.data(), count);
#else
            return ::read(::fileno(handle), buffer.data(), buffer.size());
#endif
        }

        int64_t tell_host_file(FILE* handle)
        {
#if defined(_WIN32)
            return _ftelli64(handle);
#else
            return static_cast<int64_t>(::ftello(handle));
#endif
        }

        int64_t tell_or_zero(FILE* handle, const std::string& host_path)
        {
            if (!handle || is_stdio_path(host_path))
            {
                return 0;
            }

            const auto position = std::ftell(handle);
            if (position < 0)
            {
                throw std::runtime_error("Failed to serialize fd position for '" + host_path + "'");
            }

            return static_cast<int64_t>(position);
        }

        void validate_serializable_fd(const int fd_number, const posix_fd& fd)
        {
            if (fd_number < 0)
            {
                throw std::runtime_error("fd snapshot found an invalid descriptor number " + std::to_string(fd_number));
            }

            switch (fd.type)
            {
            case fd_type::pipe_read:
            case fd_type::pipe_write:
                throw std::runtime_error("fd snapshot does not support live pipe descriptors");
            case fd_type::file:
                if (fd.host_path.empty())
                {
                    throw std::runtime_error("fd snapshot cannot serialize unnamed host file descriptor " + std::to_string(fd_number));
                }
                break;
            case fd_type::memory_file:
                if (!fd.memory_file)
                {
                    throw std::runtime_error("fd snapshot found an invalid memory file descriptor " + std::to_string(fd_number));
                }
                break;
            case fd_type::socket:
                throw std::runtime_error("fd snapshot does not support socket descriptor " + std::to_string(fd_number));
            case fd_type::eventfd:
            case fd_type::directory:
            case fd_type::epoll:
                break;
            default:
                throw std::runtime_error("fd snapshot found an invalid descriptor type for fd " + std::to_string(fd_number));
            }
        }

        void serialize_file(utils::buffer_serializer& buffer, const posix_fd& fd)
        {
            buffer.write(fd.type);
            buffer.write(fd.host_path);
            buffer.write(fd.guest_path);
            buffer.write(fd.flags);
            buffer.write(fd.directory_position);
            buffer.write(fd.read_only_mapping);

            if (fd.type == fd_type::memory_file)
            {
                buffer.write(fd.memory_file->content);
                buffer.write<uint64_t>(fd.memory_file->offset);
            }
            else
            {
                buffer.write<int64_t>(tell_or_zero(fd.handle, fd.host_path));
            }
        }

        FILE* reopen_host_file(const std::string& host_path, const int flags, const int64_t offset)
        {
            if (auto* stdio_handle = stdio_handle_for_path(host_path))
            {
                return stdio_handle;
            }

            const auto host_fd = open_snapshot_host_file(host_path, flags);
            if (host_fd < 0)
            {
                throw std::runtime_error("Failed to reopen fd snapshot host file '" + host_path + "'");
            }

            auto* handle = fdopen_snapshot_host_file(host_fd, flags);
            if (!handle)
            {
                close_snapshot_host_file(host_fd);
                throw std::runtime_error("Failed to reopen fd snapshot host file '" + host_path + "'");
            }

            setvbuf(handle, nullptr, _IONBF, 0);
            if (offset != 0 && !seek_host_file(handle, offset))
            {
                std::fclose(handle);
                throw std::runtime_error("Failed to restore fd snapshot offset for '" + host_path + "'");
            }

            return handle;
        }

        posix_fd deserialize_file(utils::buffer_deserializer& buffer)
        {
            posix_fd fd{};
            buffer.read(fd.type);
            buffer.read(fd.host_path);
            buffer.read(fd.guest_path);
            buffer.read(fd.flags);
            buffer.read(fd.directory_position);
            buffer.read(fd.read_only_mapping);

            if (fd.type == fd_type::memory_file)
            {
                fd.memory_file = std::make_shared<posix_memory_fd>();
                buffer.read(fd.memory_file->content);
                fd.memory_file->offset = static_cast<size_t>(buffer.read<uint64_t>());
                if (fd.memory_file->offset > fd.memory_file->content.size())
                {
                    throw std::runtime_error("fd snapshot has an invalid memory file offset");
                }
            }
            else
            {
                const auto offset = buffer.read<int64_t>();
                if (fd.type == fd_type::file)
                {
                    fd.handle = reopen_host_file(fd.host_path, fd.flags, offset);
                }
            }

            return fd;
        }
    }

    size_t posix_fd::read_at(const uint64_t position, const std::span<std::byte> buffer) const
    {
        if (!this->handle || !std::in_range<int64_t>(position))
        {
            return 0;
        }

        const auto saved_position = tell_host_file(this->handle);
        if (saved_position < 0 || !seek_host_file(this->handle, static_cast<int64_t>(position)))
        {
            return 0;
        }

        const auto bytes_read = std::fread(buffer.data(), 1, buffer.size(), this->handle);
        seek_host_file(this->handle, saved_position);
        return bytes_read;
    }

    // Standard streams are read through their descriptor, so a read returns what the stream has, as on a pipe or
    // terminal; fread would wait until the whole buffer is filled.
    std::optional<size_t> posix_fd::read(const std::span<std::byte> buffer) const
    {
        if (!this->handle)
        {
            return std::nullopt;
        }

        if (this->is_standard_stream())
        {
            const auto bytes_read = read_host_descriptor(this->handle, buffer);
            return bytes_read < 0 ? std::nullopt : std::optional{static_cast<size_t>(bytes_read)};
        }

        const auto bytes_read = std::fread(buffer.data(), 1, buffer.size(), this->handle);
        if (bytes_read < buffer.size() && std::ferror(this->handle))
        {
            std::clearerr(this->handle);
            return std::nullopt;
        }

        return bytes_read;
    }

    std::optional<uint64_t> posix_fd::seek(const int64_t offset, const int origin) const
    {
        if (!this->handle || this->is_standard_stream() || !seek_host_file(this->handle, offset, origin))
        {
            return std::nullopt;
        }

        const auto position = tell_host_file(this->handle);
        return position < 0 ? std::nullopt : std::optional{static_cast<uint64_t>(position)};
    }

    FILE* open_host_file(const std::filesystem::path& path, const std::string_view mode)
    {
#if defined(_WIN32)
        const std::wstring wide_mode(mode.begin(), mode.end());
        return _wfopen(path.c_str(), wide_mode.c_str());
#else
        return std::fopen(path.c_str(), std::string{mode}.c_str());
#endif
    }

    void posix_fd_table::serialize(utils::buffer_serializer& buffer) const
    {
        std::vector<const posix_fd*> files{};
        std::map<const posix_fd*, uint64_t> file_indices{};
        for (const auto& [fd_number, entry] : this->fds_)
        {
            validate_serializable_fd(fd_number, *entry.file);
            if (file_indices.emplace(entry.file.get(), files.size()).second)
            {
                files.push_back(entry.file.get());
            }
        }

        buffer.write<uint64_t>(files.size());
        for (const auto* file : files)
        {
            serialize_file(buffer, *file);
        }

        buffer.write<uint64_t>(this->fds_.size());
        for (const auto& [fd_number, entry] : this->fds_)
        {
            buffer.write(fd_number);
            buffer.write<uint64_t>(file_indices.at(entry.file.get()));
            buffer.write(entry.close_on_exec);
        }
    }

    void posix_fd_table::deserialize(utils::buffer_deserializer& buffer)
    {
        const auto file_count = buffer.read<uint64_t>();
        std::vector<std::shared_ptr<posix_fd>> files{};
        files.reserve(static_cast<size_t>(file_count));
        for (uint64_t i = 0; i < file_count; ++i)
        {
            files.push_back(std::make_shared<posix_fd>(deserialize_file(buffer)));
        }

        std::map<int, descriptor> restored{};
        const auto fd_count = buffer.read<uint64_t>();
        for (uint64_t i = 0; i < fd_count; ++i)
        {
            const auto fd_number = buffer.read<int>();
            const auto file_index = buffer.read<uint64_t>();
            if (file_index >= files.size())
            {
                throw std::runtime_error("fd snapshot references an invalid open file description");
            }

            descriptor entry{.file = files[file_index]};
            buffer.read(entry.close_on_exec);
            restored.emplace(fd_number, std::move(entry));
        }

        this->fds_ = std::move(restored);
    }
} // namespace sogen
