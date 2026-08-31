#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>

#include <serialization.hpp>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sogen
{

    enum class fd_type
    {
        file,
        memory_file,
        pipe_read,
        pipe_write,
        socket,
        eventfd,
        directory,
        epoll,
        kqueue,
    };

    struct guest_memory_fd
    {
        std::string content{};
        size_t offset{};
    };

    struct guest_socket_state
    {
        int domain{};
        int type{};
        int protocol{};
        bool listening{};
        bool connected{};
        int so_reuseaddr{};
        int so_keepalive{};
        int so_reuseport{};
        int so_sndbuf{65536};
        int so_rcvbuf{65536};
        int so_error{};
        int host_socket{-1};
        uint16_t bound_port{};
        uint32_t bound_addr{};
        uint16_t peer_port{};
        uint32_t peer_addr{};

        guest_socket_state() = default;

        ~guest_socket_state()
        {
#if !defined(_WIN32)
            if (this->host_socket >= 0)
            {
                ::close(this->host_socket);
                this->host_socket = -1;
            }
#endif
        }

        guest_socket_state(const guest_socket_state&) = delete;
        guest_socket_state& operator=(const guest_socket_state&) = delete;
    };

    struct guest_fd
    {
        fd_type type{fd_type::file};
        std::string host_path{};
        std::string guest_path{};
        FILE* handle{};
        std::shared_ptr<guest_memory_fd> memory_file{};
        int flags{};
        bool close_on_exec{};
        bool read_only_mapping{};

        std::shared_ptr<guest_socket_state> socket_state{};

        ~guest_fd() = default;
        guest_fd() = default;

        guest_fd(const guest_fd&) = delete;
        guest_fd& operator=(const guest_fd&) = delete;

        guest_fd(guest_fd&& other) noexcept
            : type(other.type),
              host_path(std::move(other.host_path)),
              guest_path(std::move(other.guest_path)),
              handle(std::exchange(other.handle, nullptr)),
              memory_file(std::move(other.memory_file)),
              flags(other.flags),
              close_on_exec(other.close_on_exec),
              read_only_mapping(other.read_only_mapping),
              socket_state(std::move(other.socket_state))
        {
        }

        guest_fd& operator=(guest_fd&& other) noexcept
        {
            if (this != &other)
            {
                type = other.type;
                host_path = std::move(other.host_path);
                guest_path = std::move(other.guest_path);
                handle = std::exchange(other.handle, nullptr);
                memory_file = std::move(other.memory_file);
                flags = other.flags;
                close_on_exec = other.close_on_exec;
                read_only_mapping = other.read_only_mapping;
                socket_state = std::move(other.socket_state);
            }
            return *this;
        }
    };

    class guest_fd_table
    {
      public:
        // The guest's O_APPEND bit is personality-specific and only the guest knows it: Linux uses
        // 02000, Darwin uses 0x0008 and spends 0x0400 on O_TRUNC instead. Decoding a Darwin descriptor
        // with the Linux mask reopens an appending file truncating, and a truncating one appending.
        static constexpr int LINUX_GUEST_O_APPEND = 02000;

        explicit guest_fd_table(const int guest_append_flag = LINUX_GUEST_O_APPEND)
            : guest_append_flag_(guest_append_flag)
        {
            // Pre-populate stdin, stdout, stderr
            guest_fd fd_stdin{};
            fd_stdin.type = fd_type::file;
            fd_stdin.host_path = "/dev/stdin";
            fd_stdin.handle = stdin;
            fd_stdin.flags = 0;
            this->fds_.emplace(0, std::move(fd_stdin));

            guest_fd fd_stdout{};
            fd_stdout.type = fd_type::file;
            fd_stdout.host_path = "/dev/stdout";
            fd_stdout.handle = stdout;
            fd_stdout.flags = 1;
            this->fds_.emplace(1, std::move(fd_stdout));

            guest_fd fd_stderr{};
            fd_stderr.type = fd_type::file;
            fd_stderr.host_path = "/dev/stderr";
            fd_stderr.handle = stderr;
            fd_stderr.flags = 1;
            this->fds_.emplace(2, std::move(fd_stderr));
        }

        int allocate(guest_fd fd)
        {
            const int new_fd = this->find_lowest_available(0);
            this->fds_.emplace(new_fd, std::move(fd));
            return new_fd;
        }

        int allocate_at(int target_fd, guest_fd fd)
        {
            // Close existing if present
            this->close(target_fd);
            this->fds_.emplace(target_fd, std::move(fd));
            return target_fd;
        }

        guest_fd* get(const int fd)
        {
            auto it = this->fds_.find(fd);
            if (it == this->fds_.end())
            {
                return nullptr;
            }

            return &it->second;
        }

        bool close(const int fd)
        {
            auto it = this->fds_.find(fd);
            if (it == this->fds_.end())
            {
                return false;
            }

            // Don't fclose stdin/stdout/stderr
            if (it->second.handle && it->second.handle != stdin && it->second.handle != stdout && it->second.handle != stderr)
            {
                fclose(it->second.handle);
                it->second.handle = nullptr;
            }

            this->fds_.erase(it);
            return true;
        }

        int dup_fd(const int oldfd, const int minimum_fd = 0)
        {
            auto* existing = this->get(oldfd);
            if (!existing)
            {
                return -1;
            }

            auto duplicate = duplicate_entry(*existing);
            if (!duplicate.has_value())
            {
                return -1;
            }

            const int new_fd = this->find_lowest_available(minimum_fd);
            this->fds_.emplace(new_fd, std::move(*duplicate));
            return new_fd;
        }

        int dup2_fd(const int oldfd, const int newfd)
        {
            if (oldfd == newfd)
            {
                return (this->get(oldfd) != nullptr) ? newfd : -1;
            }

            auto* existing = this->get(oldfd);
            if (!existing)
            {
                return -1;
            }

            auto duplicate = duplicate_entry(*existing);
            if (!duplicate.has_value())
            {
                return -1;
            }

            // Close newfd if open
            this->close(newfd);

            this->fds_.emplace(newfd, std::move(*duplicate));
            return newfd;
        }

        void set_close_on_exec(const int fd, const bool value)
        {
            auto* entry = this->get(fd);
            if (entry)
            {
                entry->close_on_exec = value;
            }
        }

        const std::map<int, guest_fd>& get_fds() const
        {
            return this->fds_;
        }

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::map<int, guest_fd> fds_{};
        int guest_append_flag_{LINUX_GUEST_O_APPEND};

        static const char* get_stream_mode(const guest_fd& fd)
        {
            switch (fd.type)
            {
            case fd_type::pipe_write:
                return "wb";
            case fd_type::pipe_read:
                return "rb";
            default:
                break;
            }

            if (fd.host_path == "/dev/stdout" || fd.host_path == "/dev/stderr")
            {
                return "wb";
            }

            switch (fd.flags & 3)
            {
            case 1:
                return "wb";
            case 2:
                return "r+b";
            default:
                return "rb";
            }
        }

        static FILE* duplicate_handle(FILE* handle, const char* mode)
        {
            if (!handle)
            {
                return nullptr;
            }

#if defined(_WIN32)
            const auto duplicated_fd = _dup(_fileno(handle));
            if (duplicated_fd < 0)
            {
                return nullptr;
            }

            auto* duplicated_handle = _fdopen(duplicated_fd, mode);
#else
            const auto duplicated_fd = dup(fileno(handle));
            if (duplicated_fd < 0)
            {
                return nullptr;
            }

            auto* duplicated_handle = fdopen(duplicated_fd, mode);
#endif
            if (!duplicated_handle)
            {
#if defined(_WIN32)
                _close(duplicated_fd);
#else
                ::close(duplicated_fd);
#endif
                return nullptr;
            }

            setvbuf(duplicated_handle, nullptr, _IONBF, 0);
            return duplicated_handle;
        }

        static std::optional<guest_fd> duplicate_entry(const guest_fd& existing)
        {
            guest_fd new_entry{};
            new_entry.type = existing.type;
            new_entry.host_path = existing.host_path;
            new_entry.guest_path = existing.guest_path;
            new_entry.flags = existing.flags;
            new_entry.close_on_exec = false; // dup clears close-on-exec
            new_entry.read_only_mapping = existing.read_only_mapping;

            new_entry.handle = duplicate_handle(existing.handle, get_stream_mode(existing));
            new_entry.memory_file = existing.memory_file;
            new_entry.socket_state = existing.socket_state;
            if (existing.handle && !new_entry.handle)
            {
                return std::nullopt;
            }

            return new_entry;
        }

        int find_lowest_available(const int start) const
        {
            int fd = start;
            while (this->fds_.contains(fd))
            {
                ++fd;
            }
            return fd;
        }
    };

    namespace guest_fd_detail
    {
        inline bool is_stdio_path(const std::string& path)
        {
            return path == "/dev/stdin" || path == "/dev/stdout" || path == "/dev/stderr";
        }

        inline FILE* stdio_handle_for_path(const std::string& path)
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

        inline int open_flags_for_snapshot(const int flags, const int guest_append_flag)
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
            if ((flags & guest_append_flag) != 0)
            {
                open_flags |= _O_APPEND;
            }
#else
            auto open_flags = access;
            if ((flags & guest_append_flag) != 0)
            {
                open_flags |= O_APPEND;
            }
#endif
            return open_flags;
        }

        inline const char* fdopen_mode_for_flags(const int flags, const int guest_append_flag)
        {
            const auto access = flags & 3;
            if (access == 0)
            {
                return "rb";
            }

            if ((flags & guest_append_flag) != 0)
            {
                return access == 2 ? "a+b" : "ab";
            }

            return access == 2 ? "r+b" : "wb";
        }

        inline int open_snapshot_host_file(const std::string& host_path, const int flags, const int guest_append_flag)
        {
#if defined(_WIN32)
            return _open(host_path.c_str(), open_flags_for_snapshot(flags, guest_append_flag));
#else
            return ::open(host_path.c_str(), open_flags_for_snapshot(flags, guest_append_flag));
#endif
        }

        inline FILE* fdopen_snapshot_host_file(const int fd, const int flags, const int guest_append_flag)
        {
#if defined(_WIN32)
            return _fdopen(fd, fdopen_mode_for_flags(flags, guest_append_flag));
#else
            return ::fdopen(fd, fdopen_mode_for_flags(flags, guest_append_flag));
#endif
        }

        inline void close_snapshot_host_file(const int fd)
        {
#if defined(_WIN32)
            _close(fd);
#else
            ::close(fd);
#endif
        }

        inline bool seek_snapshot_host_file(FILE* handle, const int64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(handle, offset, SEEK_SET) == 0;
#else
            return ::fseeko(handle, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        inline int64_t tell_or_zero(FILE* handle, const std::string& host_path)
        {
            if (!handle || is_stdio_path(host_path))
            {
                return 0;
            }

            const auto position = std::ftell(handle);
            if (position < 0)
            {
                throw std::runtime_error("Failed to serialize guest fd position for '" + host_path + "'");
            }

            return static_cast<int64_t>(position);
        }

        inline void close_if_owned(guest_fd& fd)
        {
            if (fd.handle && fd.handle != stdin && fd.handle != stdout && fd.handle != stderr)
            {
                std::fclose(fd.handle);
                fd.handle = nullptr;
            }
        }

        inline void close_restored(std::map<int, guest_fd>& fds)
        {
            for (auto& [_, fd] : fds)
            {
                close_if_owned(fd);
            }
        }

        inline void validate_serializable_fd(const int fd_number, const guest_fd& fd)
        {
            if (fd_number < 0)
            {
                throw std::runtime_error("Guest fd snapshot found an invalid descriptor number " + std::to_string(fd_number));
            }

            switch (fd.type)
            {
            case fd_type::pipe_read:
            case fd_type::pipe_write:
                throw std::runtime_error("Guest fd snapshot does not support live pipe descriptors");
            case fd_type::file:
                if (fd.host_path.empty())
                {
                    throw std::runtime_error("Guest fd snapshot cannot serialize unnamed host file descriptor " +
                                             std::to_string(fd_number));
                }
                break;
            case fd_type::memory_file:
                if (!fd.memory_file)
                {
                    throw std::runtime_error("Guest fd snapshot found an invalid memory file descriptor " + std::to_string(fd_number));
                }
                break;
            case fd_type::socket:
                throw std::runtime_error("Guest fd snapshot does not support socket descriptor " + std::to_string(fd_number));
            case fd_type::kqueue:
                throw std::runtime_error("Guest fd snapshot does not support kqueue descriptor " + std::to_string(fd_number));
            case fd_type::eventfd:
            case fd_type::directory:
            case fd_type::epoll:
                break;
            default:
                throw std::runtime_error("Guest fd snapshot found an invalid descriptor type for fd " + std::to_string(fd_number));
            }
        }

    }

    inline void guest_fd_table::serialize(utils::buffer_serializer& buffer) const
    {
        for (const auto& [fd_number, fd] : this->fds_)
        {
            guest_fd_detail::validate_serializable_fd(fd_number, fd);
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
                buffer.write<int64_t>(guest_fd_detail::tell_or_zero(fd.handle, fd.host_path));
            }
        }
    }

    inline void guest_fd_table::deserialize(utils::buffer_deserializer& buffer)
    {
        std::map<int, guest_fd> restored{};
        try
        {
            const auto count = buffer.read<uint64_t>();
            for (uint64_t i = 0; i < count; ++i)
            {
                const auto fd_number = buffer.read<int>();
                guest_fd fd{};
                buffer.read(fd.type);
                buffer.read(fd.host_path);
                buffer.read(fd.guest_path);
                buffer.read(fd.flags);
                buffer.read(fd.close_on_exec);
                buffer.read(fd.read_only_mapping);

                if (fd.type == fd_type::memory_file)
                {
                    fd.memory_file = std::make_shared<guest_memory_fd>();
                    buffer.read(fd.memory_file->content);
                    fd.memory_file->offset = static_cast<size_t>(buffer.read<uint64_t>());
                    if (fd.memory_file->offset > fd.memory_file->content.size())
                    {
                        throw std::runtime_error("Guest fd snapshot has an invalid memory file offset");
                    }
                }
                else
                {
                    const auto offset = buffer.read<int64_t>();
                    if (fd.type == fd_type::file)
                    {
                        if (auto* stdio_handle = guest_fd_detail::stdio_handle_for_path(fd.host_path))
                        {
                            fd.handle = stdio_handle;
                        }
                        else
                        {
                            const auto host_fd = guest_fd_detail::open_snapshot_host_file(fd.host_path, fd.flags, this->guest_append_flag_);
                            if (host_fd < 0)
                            {
                                throw std::runtime_error("Failed to reopen guest fd snapshot host file '" + fd.host_path + "'");
                            }

                            fd.handle = guest_fd_detail::fdopen_snapshot_host_file(host_fd, fd.flags, this->guest_append_flag_);
                            if (!fd.handle)
                            {
                                const auto error = errno;
                                guest_fd_detail::close_snapshot_host_file(host_fd);
                                errno = error;
                            }
                            if (!fd.handle)
                            {
                                throw std::runtime_error("Failed to reopen guest fd snapshot host file '" + fd.host_path + "'");
                            }
                            setvbuf(fd.handle, nullptr, _IONBF, 0);
                            if (offset != 0 && !guest_fd_detail::seek_snapshot_host_file(fd.handle, offset))
                            {
                                throw std::runtime_error("Failed to restore guest fd snapshot offset for '" + fd.host_path + "'");
                            }
                        }
                    }
                }

                restored.emplace(fd_number, std::move(fd));
            }
        }
        catch (...)
        {
            guest_fd_detail::close_restored(restored);
            throw;
        }

        for (auto& [_, fd] : this->fds_)
        {
            guest_fd_detail::close_if_owned(fd);
        }
        this->fds_ = std::move(restored);
    }
} // namespace sogen
