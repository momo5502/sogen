#include "posix_path_syscalls.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

namespace sogen
{
    namespace
    {
        bool has_any_execute_permission(const std::filesystem::perms permissions)
        {
            constexpr auto any_execute =
                std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
            return (permissions & any_execute) != std::filesystem::perms::none;
        }
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
        std::error_code ec{};
        const auto status = std::filesystem::status(file_system.translate(guest_path), ec);
        if (!std::filesystem::exists(status))
        {
            return posix_errno::enoent;
        }

        if (!std::filesystem::is_directory(status))
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
        std::error_code ec{};
        const auto status = std::filesystem::status(file_system.translate(guest_path), ec);
        if (!std::filesystem::exists(status))
        {
            return posix_errno::enoent;
        }

        if ((mode & POSIX_W_OK) && file_system.is_read_only_guest_path(guest_path))
        {
            return posix_errno::erofs;
        }

        if ((mode & POSIX_X_OK) && !has_any_execute_permission(status.permissions()))
        {
            return posix_errno::eacces;
        }

        return uint64_t{0};
    }
}
