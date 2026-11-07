#include "posix_descriptor_syscalls.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <optional>

namespace sogen
{
    namespace
    {
        // Linux and XNU number SEEK_SET, SEEK_CUR and SEEK_END alike.
        constexpr std::array host_seek_origins{SEEK_SET, SEEK_CUR, SEEK_END};

        void remove_from_epoll_entries(posix_fd_table& fds, const int fd)
        {
            fds.for_each_epoll_description([fd](posix_fd& epoll) {
                std::erase_if(epoll.epoll_entries, [fd](const posix_epoll_entry& entry) { return entry.fd == fd; });
            });
        }

        std::optional<uint64_t> moved_position(const uint64_t base, const int64_t offset)
        {
            constexpr auto max_position = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

            if (offset < 0)
            {
                const auto distance = static_cast<uint64_t>(-(offset + 1)) + 1;
                return distance > base ? std::nullopt : std::optional{base - distance};
            }

            const auto distance = static_cast<uint64_t>(offset);
            return base > max_position || distance > max_position - base ? std::nullopt : std::optional{base + distance};
        }

        uint64_t memory_file_seek_base(const posix_memory_fd& memory_file, const int origin)
        {
            switch (origin)
            {
            case SEEK_CUR:
                return memory_file.offset;
            case SEEK_END:
                return memory_file.content.size();
            default:
                return 0;
            }
        }

        std::optional<uint64_t> seek_memory_file(posix_memory_fd& memory_file, const int64_t offset, const int origin)
        {
            const auto position = moved_position(memory_file_seek_base(memory_file, origin), offset);
            if (position)
            {
                memory_file.offset = static_cast<size_t>(*position);
            }

            return position;
        }

        // A directory position is an index into its listing, so there is no end to seek from.
        std::optional<uint64_t> seek_directory(posix_fd& directory, const int64_t offset, const int origin)
        {
            if (origin == SEEK_END)
            {
                return std::nullopt;
            }

            const auto position = moved_position(origin == SEEK_SET ? 0 : directory.directory_position, offset);
            if (position)
            {
                directory.directory_position = *position;
            }

            return position;
        }

        bool is_seekable(const posix_fd& file)
        {
            switch (file.type)
            {
            case fd_type::file:
                return !file.is_standard_stream();
            case fd_type::memory_file:
            case fd_type::directory:
                return true;
            default:
                return false;
            }
        }

        std::optional<uint64_t> seek(posix_fd& file, const int64_t offset, const int origin)
        {
            switch (file.type)
            {
            case fd_type::memory_file:
                return file.memory_file ? seek_memory_file(*file.memory_file, offset, origin) : std::nullopt;
            case fd_type::directory:
                return seek_directory(file, offset, origin);
            default:
                return file.seek(offset, origin);
            }
        }
    }

    posix_result posix_close(posix_fd_table& fds, const int fd)
    {
        if (!fds.close(fd))
        {
            return posix_errno::ebadf;
        }

        remove_from_epoll_entries(fds, fd);
        return uint64_t{0};
    }

    posix_result posix_dup(posix_fd_table& fds, const int fd, const int64_t minimum_fd, const bool close_on_exec,
                           const int descriptor_limit)
    {
        if (fds.get(fd) == nullptr)
        {
            return posix_errno::ebadf;
        }

        if (minimum_fd < 0 || minimum_fd >= descriptor_limit)
        {
            return posix_errno::einval;
        }

        const auto new_fd = fds.dup_fd(fd, static_cast<int>(minimum_fd));
        if (new_fd >= descriptor_limit)
        {
            fds.close(new_fd);
            return posix_errno::emfile;
        }

        fds.set_close_on_exec(new_fd, close_on_exec);
        return static_cast<uint64_t>(new_fd);
    }

    posix_result posix_dup2(posix_fd_table& fds, const int fd, const int new_fd, const int descriptor_limit)
    {
        if (fds.get(fd) == nullptr || new_fd < 0 || new_fd >= descriptor_limit)
        {
            return posix_errno::ebadf;
        }

        const auto replaces_open_descriptor = fd != new_fd && fds.get(new_fd) != nullptr;
        fds.dup2_fd(fd, new_fd);
        if (replaces_open_descriptor)
        {
            remove_from_epoll_entries(fds, new_fd);
        }

        return static_cast<uint64_t>(new_fd);
    }

    posix_result posix_get_descriptor_flags(const posix_fd_table& fds, const int fd)
    {
        if (fds.get(fd) == nullptr)
        {
            return posix_errno::ebadf;
        }

        return fds.is_close_on_exec(fd) ? POSIX_FD_CLOEXEC : 0;
    }

    posix_result posix_set_descriptor_flags(posix_fd_table& fds, const int fd, const uint64_t flags)
    {
        if (fds.get(fd) == nullptr)
        {
            return posix_errno::ebadf;
        }

        fds.set_close_on_exec(fd, (flags & POSIX_FD_CLOEXEC) != 0);
        return uint64_t{0};
    }

    posix_result posix_get_status_flags(const posix_fd_table& fds, const int fd)
    {
        const auto* file = fds.get(fd);
        if (file == nullptr)
        {
            return posix_errno::ebadf;
        }

        return static_cast<uint64_t>(file->flags);
    }

    posix_result posix_lseek(posix_fd_table& fds, const int fd, const int64_t offset, const uint64_t whence)
    {
        auto* file = fds.get(fd);
        if (file == nullptr)
        {
            return posix_errno::ebadf;
        }

        if (!is_seekable(*file))
        {
            return posix_errno::espipe;
        }

        if (whence >= host_seek_origins.size())
        {
            return posix_errno::einval;
        }

        const auto position = seek(*file, offset, host_seek_origins.at(static_cast<size_t>(whence)));
        if (!position)
        {
            return posix_errno::einval;
        }

        return *position;
    }
}
