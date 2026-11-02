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
        bool read_only_mapping{};

        std::shared_ptr<posix_socket_state> socket_state{};

        bool is_standard_stream() const
        {
            return this->handle == stdin || this->handle == stdout || this->handle == stderr;
        }

        size_t read_at(uint64_t position, std::span<std::byte> buffer) const;
        std::optional<size_t> read(std::span<std::byte> buffer) const;
        std::optional<uint64_t> seek(int64_t offset, int origin) const;

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

    // A descriptor refers to an open file description that dup shares, so the file position, directory position and
    // status flags are shared between duplicates while close-on-exec stays per descriptor.
    class posix_fd_table
    {
      public:
        posix_fd_table()
        {
            this->fds_.emplace(0, descriptor{.file = make_standard_stream("/dev/stdin", stdin, 0)});
            this->fds_.emplace(1, descriptor{.file = make_standard_stream("/dev/stdout", stdout, 1)});
            this->fds_.emplace(2, descriptor{.file = make_standard_stream("/dev/stderr", stderr, 1)});
        }

        int allocate(posix_fd fd, const bool close_on_exec = false)
        {
            const int new_fd = this->find_lowest_available(0);
            this->fds_.emplace(new_fd, descriptor{.file = std::make_shared<posix_fd>(std::move(fd)), .close_on_exec = close_on_exec});
            return new_fd;
        }

        int allocate_at(const int target_fd, posix_fd fd, const bool close_on_exec = false)
        {
            this->fds_.insert_or_assign(target_fd,
                                        descriptor{.file = std::make_shared<posix_fd>(std::move(fd)), .close_on_exec = close_on_exec});
            return target_fd;
        }

        posix_fd* get(const int fd)
        {
            const auto it = this->fds_.find(fd);
            return it == this->fds_.end() ? nullptr : it->second.file.get();
        }

        const posix_fd* get(const int fd) const
        {
            const auto it = this->fds_.find(fd);
            return it == this->fds_.end() ? nullptr : it->second.file.get();
        }

        bool close(const int fd)
        {
            return this->fds_.erase(fd) != 0;
        }

        int dup_fd(const int oldfd, const int minimum_fd = 0)
        {
            const auto it = this->fds_.find(oldfd);
            if (it == this->fds_.end())
            {
                return -1;
            }

            const int new_fd = this->find_lowest_available(minimum_fd);
            this->fds_.emplace(new_fd, descriptor{.file = it->second.file});
            return new_fd;
        }

        int dup2_fd(const int oldfd, const int newfd)
        {
            const auto it = this->fds_.find(oldfd);
            if (it == this->fds_.end())
            {
                return -1;
            }

            if (oldfd != newfd)
            {
                this->fds_.insert_or_assign(newfd, descriptor{.file = it->second.file});
            }

            return newfd;
        }

        bool is_close_on_exec(const int fd) const
        {
            const auto it = this->fds_.find(fd);
            return it != this->fds_.end() && it->second.close_on_exec;
        }

        void set_close_on_exec(const int fd, const bool value)
        {
            const auto it = this->fds_.find(fd);
            if (it != this->fds_.end())
            {
                it->second.close_on_exec = value;
            }
        }

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        struct descriptor
        {
            std::shared_ptr<posix_fd> file{};
            bool close_on_exec{};
        };

        std::map<int, descriptor> fds_{};

        static std::shared_ptr<posix_fd> make_standard_stream(std::string host_path, FILE* handle, const int flags)
        {
            auto stream = std::make_shared<posix_fd>();
            stream->type = fd_type::file;
            stream->host_path = std::move(host_path);
            stream->handle = handle;
            stream->flags = flags;
            return stream;
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
