#include "std_include.hpp"
#include "linux_fd_table.hpp"

#include <cerrno>
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

        bool seek_snapshot_host_file(FILE* handle, const int64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(handle, offset, SEEK_SET) == 0;
#else
            return ::fseeko(handle, static_cast<off_t>(offset), SEEK_SET) == 0;
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
                throw std::runtime_error("Failed to serialize Linux fd position for '" + host_path + "'");
            }

            return static_cast<int64_t>(position);
        }

        void close_if_owned(linux_fd& fd)
        {
            if (fd.handle && fd.handle != stdin && fd.handle != stdout && fd.handle != stderr)
            {
                std::fclose(fd.handle);
                fd.handle = nullptr;
            }
        }

        void close_restored(std::map<int, linux_fd>& fds)
        {
            for (auto& [_, fd] : fds)
            {
                close_if_owned(fd);
            }
        }

        void validate_serializable_fd(const int fd_number, const linux_fd& fd)
        {
            if (fd_number < 0)
            {
                throw std::runtime_error("Linux fd snapshot found an invalid descriptor number " + std::to_string(fd_number));
            }

            switch (fd.type)
            {
            case fd_type::pipe_read:
            case fd_type::pipe_write:
                throw std::runtime_error("Linux fd snapshot does not support live pipe descriptors");
            case fd_type::file:
                if (fd.host_path.empty())
                {
                    throw std::runtime_error("Linux fd snapshot cannot serialize unnamed host file descriptor " +
                                             std::to_string(fd_number));
                }
                break;
            case fd_type::memory_file:
                if (!fd.memory_file)
                {
                    throw std::runtime_error("Linux fd snapshot found an invalid memory file descriptor " + std::to_string(fd_number));
                }
                break;
            case fd_type::socket:
                throw std::runtime_error("Linux fd snapshot does not support socket descriptor " + std::to_string(fd_number));
            case fd_type::eventfd:
            case fd_type::directory:
            case fd_type::epoll:
                break;
            default:
                throw std::runtime_error("Linux fd snapshot found an invalid descriptor type for fd " + std::to_string(fd_number));
            }
        }

    }

    void linux_fd_table::serialize(utils::buffer_serializer& buffer) const
    {
        for (const auto& [fd_number, fd] : this->fds_)
        {
            validate_serializable_fd(fd_number, fd);
        }

        buffer.write<uint64_t>(this->fds_.size());

        for (const auto& [fd_number, fd] : this->fds_)
        {
            buffer.write(fd_number);
            buffer.write(fd.type);
            buffer.write(fd.host_path);
            buffer.write(fd.guest_path);
            buffer.write(fd.flags);
            buffer.write(fd.close_on_exec);
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
    }

    void linux_fd_table::deserialize(utils::buffer_deserializer& buffer)
    {
        std::map<int, linux_fd> restored{};
        try
        {
            const auto count = buffer.read<uint64_t>();
            for (uint64_t i = 0; i < count; ++i)
            {
                const auto fd_number = buffer.read<int>();
                linux_fd fd{};
                buffer.read(fd.type);
                buffer.read(fd.host_path);
                buffer.read(fd.guest_path);
                buffer.read(fd.flags);
                buffer.read(fd.close_on_exec);
                buffer.read(fd.read_only_mapping);

                if (fd.type == fd_type::memory_file)
                {
                    fd.memory_file = std::make_shared<linux_memory_fd>();
                    buffer.read(fd.memory_file->content);
                    fd.memory_file->offset = static_cast<size_t>(buffer.read<uint64_t>());
                    if (fd.memory_file->offset > fd.memory_file->content.size())
                    {
                        throw std::runtime_error("Linux fd snapshot has an invalid memory file offset");
                    }
                }
                else
                {
                    const auto offset = buffer.read<int64_t>();
                    if (fd.type == fd_type::file)
                    {
                        if (auto* stdio_handle = stdio_handle_for_path(fd.host_path))
                        {
                            fd.handle = stdio_handle;
                        }
                        else
                        {
                            const auto host_fd = open_snapshot_host_file(fd.host_path, fd.flags);
                            if (host_fd < 0)
                            {
                                throw std::runtime_error("Failed to reopen Linux fd snapshot host file '" + fd.host_path + "'");
                            }

                            fd.handle = fdopen_snapshot_host_file(host_fd, fd.flags);
                            if (!fd.handle)
                            {
                                const auto error = errno;
                                close_snapshot_host_file(host_fd);
                                errno = error;
                            }
                            if (!fd.handle)
                            {
                                throw std::runtime_error("Failed to reopen Linux fd snapshot host file '" + fd.host_path + "'");
                            }
                            setvbuf(fd.handle, nullptr, _IONBF, 0);
                            if (offset != 0 && !seek_snapshot_host_file(fd.handle, offset))
                            {
                                throw std::runtime_error("Failed to restore Linux fd snapshot offset for '" + fd.host_path + "'");
                            }
                        }
                    }
                }

                restored.emplace(fd_number, std::move(fd));
            }
        }
        catch (...)
        {
            close_restored(restored);
            throw;
        }

        for (auto& [_, fd] : this->fds_)
        {
            close_if_owned(fd);
        }
        this->fds_ = std::move(restored);
    }
} // namespace sogen
