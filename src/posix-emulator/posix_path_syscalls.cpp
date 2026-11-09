#include "posix_path_syscalls.hpp"
#include "posix_stat.hpp"

#include <utility>

namespace sogen
{
    namespace
    {
        constexpr uint32_t ANY_EXECUTE_PERMISSION = 0111;
    }

    posix_errno posix_errno_of(const posix_path_error error)
    {
        return error == posix_path_error::not_a_directory ? posix_errno::enotdir : posix_errno::ebadf;
    }

    std::variant<std::string, posix_errno> posix_resolve_path_at(const posix_fd_table& fds, const std::string_view working_directory,
                                                                 const std::optional<int> directory_fd,
                                                                 const std::string_view requested_path)
    {
        if (requested_path.empty())
        {
            return posix_errno::enoent;
        }

        posix_path_error error{};
        auto guest_path = posix_resolve_guest_path_at(fds, working_directory, directory_fd, requested_path, error);
        if (!guest_path)
        {
            return posix_errno_of(error);
        }

        return std::move(*guest_path);
    }

    posix_result posix_chdir(const posix_file_system& file_system, std::string& working_directory, std::string guest_path)
    {
        const auto status = posix_stat_path(file_system, guest_path, true);
        if (const auto* error = std::get_if<posix_errno>(&status))
        {
            return *error;
        }

        if (std::get<posix_file_status>(status).file_type() != POSIX_S_IFDIR)
        {
            return posix_errno::enotdir;
        }

        working_directory = std::move(guest_path);
        return uint64_t{0};
    }

    posix_result posix_fchdir(const posix_fd_table& fds, const int fd, std::string& working_directory)
    {
        const auto* directory = fds.get(fd);
        if (!directory)
        {
            return posix_errno::ebadf;
        }

        if (directory->type != fd_type::directory)
        {
            return posix_errno::enotdir;
        }

        if (directory->guest_path.empty())
        {
            return posix_errno::ebadf;
        }

        working_directory = posix_file_system::normalize_guest_path_string(directory->guest_path);
        return uint64_t{0};
    }

    posix_result posix_access(const posix_file_system& file_system, const std::string& guest_path, const uint64_t mode)
    {
        const auto status = posix_stat_path(file_system, guest_path, true);
        if (const auto* error = std::get_if<posix_errno>(&status))
        {
            return *error;
        }

        if ((mode & POSIX_W_OK) && file_system.is_read_only_guest_path(guest_path))
        {
            return posix_errno::erofs;
        }

        if ((mode & POSIX_X_OK) && (std::get<posix_file_status>(status).mode & ANY_EXECUTE_PERMISSION) == 0)
        {
            return posix_errno::eacces;
        }

        return uint64_t{0};
    }
}
