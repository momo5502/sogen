#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <serialization.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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
    };

    struct posix_memory_fd
    {
        std::string content{};
        size_t offset{};
    };

    struct posix_socket_state
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

        posix_socket_state() = default;

        ~posix_socket_state()
        {
#if !defined(_WIN32)
            if (this->host_socket >= 0)
            {
                ::close(this->host_socket);
                this->host_socket = -1;
            }
#endif
        }

        posix_socket_state(const posix_socket_state&) = delete;
        posix_socket_state& operator=(const posix_socket_state&) = delete;
    };

    struct posix_fd
    {
        fd_type type{fd_type::file};
        std::string host_path{};
        std::string guest_path{};
        FILE* handle{};
        std::shared_ptr<posix_memory_fd> memory_file{};
        int flags{};
        uint64_t directory_position{};
        bool close_on_exec{};
        bool read_only_mapping{};

        std::shared_ptr<posix_socket_state> socket_state{};

        bool is_standard_stream() const
        {
            return this->handle == stdin || this->handle == stdout || this->handle == stderr;
        }

        size_t read_at(uint64_t position, std::span<std::byte> buffer) const;

        ~posix_fd()
        {
            this->close_handle();
        }

        posix_fd() = default;

        posix_fd(const posix_fd&) = delete;
        posix_fd& operator=(const posix_fd&) = delete;

        posix_fd(posix_fd&& other) noexcept
            : type(other.type),
              host_path(std::move(other.host_path)),
              guest_path(std::move(other.guest_path)),
              handle(std::exchange(other.handle, nullptr)),
              memory_file(std::move(other.memory_file)),
              flags(other.flags),
              directory_position(other.directory_position),
              close_on_exec(other.close_on_exec),
              read_only_mapping(other.read_only_mapping),
              socket_state(std::move(other.socket_state))
        {
        }

        posix_fd& operator=(posix_fd&& other) noexcept
        {
            if (this != &other)
            {
                this->close_handle();
                type = other.type;
                host_path = std::move(other.host_path);
                guest_path = std::move(other.guest_path);
                handle = std::exchange(other.handle, nullptr);
                memory_file = std::move(other.memory_file);
                flags = other.flags;
                directory_position = other.directory_position;
                close_on_exec = other.close_on_exec;
                read_only_mapping = other.read_only_mapping;
                socket_state = std::move(other.socket_state);
            }
            return *this;
        }

      private:
        void close_handle()
        {
            if (this->handle && !this->is_standard_stream())
            {
                std::fclose(this->handle);
            }

            this->handle = nullptr;
        }
    };

    FILE* open_host_file(const std::filesystem::path& path, std::string_view mode);

    class posix_fd_table
    {
      public:
        posix_fd_table()
        {
            // Pre-populate stdin, stdout, stderr
            posix_fd fd_stdin{};
            fd_stdin.type = fd_type::file;
            fd_stdin.host_path = "/dev/stdin";
            fd_stdin.handle = stdin;
            fd_stdin.flags = 0;
            this->fds_.emplace(0, std::move(fd_stdin));

            posix_fd fd_stdout{};
            fd_stdout.type = fd_type::file;
            fd_stdout.host_path = "/dev/stdout";
            fd_stdout.handle = stdout;
            fd_stdout.flags = 1;
            this->fds_.emplace(1, std::move(fd_stdout));

            posix_fd fd_stderr{};
            fd_stderr.type = fd_type::file;
            fd_stderr.host_path = "/dev/stderr";
            fd_stderr.handle = stderr;
            fd_stderr.flags = 1;
            this->fds_.emplace(2, std::move(fd_stderr));
        }

        int allocate(posix_fd fd)
        {
            const int new_fd = this->find_lowest_available(0);
            this->fds_.emplace(new_fd, std::move(fd));
            return new_fd;
        }

        int allocate_at(int target_fd, posix_fd fd)
        {
            // Close existing if present
            this->close(target_fd);
            this->fds_.emplace(target_fd, std::move(fd));
            return target_fd;
        }

        posix_fd* get(const int fd)
        {
            auto it = this->fds_.find(fd);
            if (it == this->fds_.end())
            {
                return nullptr;
            }

            return &it->second;
        }

        const posix_fd* get(const int fd) const
        {
            const auto it = this->fds_.find(fd);
            return it == this->fds_.end() ? nullptr : &it->second;
        }

        bool close(const int fd)
        {
            return this->fds_.erase(fd) != 0;
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

        const std::map<int, posix_fd>& get_fds() const
        {
            return this->fds_;
        }

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::map<int, posix_fd> fds_{};

        static const char* get_stream_mode(const posix_fd& fd)
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

        static std::optional<posix_fd> duplicate_entry(const posix_fd& existing)
        {
            posix_fd new_entry{};
            new_entry.type = existing.type;
            new_entry.host_path = existing.host_path;
            new_entry.guest_path = existing.guest_path;
            new_entry.flags = existing.flags;
            new_entry.directory_position = existing.directory_position;
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

} // namespace sogen
