#include "posix_stat.hpp"
#include "posix_host_error.hpp"

#include <cerrno>
#include <system_error>
#include <sys/stat.h>

#if defined(_WIN32)
#include <utils/win.hpp>
#endif

namespace sogen
{
    namespace
    {
        constexpr uint32_t PIPE_PERMISSIONS = 0600;
        constexpr uint32_t SOCKET_PERMISSIONS = 0777;
        constexpr uint32_t MEMORY_FILE_PERMISSIONS = 0444;
        constexpr uint32_t ANONYMOUS_INODE_PERMISSIONS = 0600;

#if defined(_WIN32)
        using host_stat = struct ::_stat64;

        int stat_host(const std::filesystem::path& path, host_stat& status, bool /*follow_symlinks*/)
        {
            return _wstat64(path.c_str(), &status);
        }

        posix_timespec to_timespec(const __time64_t seconds)
        {
            return {.seconds = seconds, .nanoseconds = 0};
        }

        // The C runtime reports neither a device nor an inode number; the volume serial and file index stand for them.
        void apply_file_id(const std::filesystem::path& path, posix_file_status& status)
        {
            auto* const handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return;
            }

            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileInformationByHandle(handle, &info))
            {
                status.device = info.dwVolumeSerialNumber;
                status.inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
            }

            CloseHandle(handle);
        }

        // The C runtime grants execute permission only to .exe, .com, .bat and .cmd files, which would leave guest
        // executables unrunnable; std::filesystem derives the permissions from the read-only attribute alone.
        void apply_permissions(const std::filesystem::path& path, posix_file_status& status)
        {
            std::error_code ec{};
            const auto permissions = std::filesystem::status(path, ec).permissions();
            if (ec || permissions == std::filesystem::perms::unknown)
            {
                return;
            }

            status.mode = status.file_type() | (static_cast<uint32_t>(permissions) & POSIX_PERMISSION_BITS);
        }

        void apply_host_details(const std::filesystem::path& path, posix_file_status& status)
        {
            apply_file_id(path, status);
            apply_permissions(path, status);
        }
#else
        using host_stat = struct stat;

        int stat_host(const std::filesystem::path& path, host_stat& status, const bool follow_symlinks)
        {
            return follow_symlinks ? ::stat(path.c_str(), &status) : ::lstat(path.c_str(), &status);
        }

        posix_timespec to_timespec(const timespec& time)
        {
            return {.seconds = time.tv_sec, .nanoseconds = time.tv_nsec};
        }

        void apply_host_details(const std::filesystem::path& /*path*/, posix_file_status& /*status*/)
        {
        }
#endif

        posix_file_status to_file_status(const host_stat& host)
        {
            posix_file_status status{};
            status.mode = static_cast<uint32_t>(host.st_mode) & (POSIX_S_IFMT | POSIX_PERMISSION_BITS);
            status.device = static_cast<uint64_t>(host.st_dev);
            status.inode = static_cast<uint64_t>(host.st_ino);
            status.link_count = static_cast<uint64_t>(host.st_nlink);
            status.user_id = static_cast<uint32_t>(host.st_uid);
            status.group_id = static_cast<uint32_t>(host.st_gid);
            status.special_device = static_cast<uint64_t>(host.st_rdev);
            status.size = static_cast<int64_t>(host.st_size);
#if defined(_WIN32)
            status.access_time = to_timespec(host.st_atime);
            status.modification_time = to_timespec(host.st_mtime);
            status.change_time = to_timespec(host.st_ctime);
#elif defined(__APPLE__)
            status.access_time = to_timespec(host.st_atimespec);
            status.modification_time = to_timespec(host.st_mtimespec);
            status.change_time = to_timespec(host.st_ctimespec);
#else
            status.access_time = to_timespec(host.st_atim);
            status.modification_time = to_timespec(host.st_mtim);
            status.change_time = to_timespec(host.st_ctim);
#endif
            return status;
        }

        posix_file_status anonymous_status(const uint32_t mode)
        {
            return {.mode = mode, .link_count = 1};
        }
    }

    posix_stat_result posix_stat_host_path(const std::filesystem::path& host_path, const bool follow_symlinks)
    {
        host_stat host{};
        if (stat_host(host_path, host, follow_symlinks) != 0)
        {
            return posix_errno_from_host(errno);
        }

        auto status = to_file_status(host);
        apply_host_details(host_path, status);
        return status;
    }

    posix_stat_result posix_stat_path(const posix_file_system& file_system, const std::string& guest_path, const bool follow_symlinks)
    {
        return posix_stat_host_path(file_system.translate(guest_path), follow_symlinks);
    }

    // The standard streams are presented as pipes, as when output is captured, so runs do not depend on whether the
    // host has a terminal or what its /dev/stdin stands for.
    posix_stat_result posix_stat_descriptor(const posix_fd& file)
    {
        if (file.is_standard_stream())
        {
            return anonymous_status(POSIX_S_IFIFO | PIPE_PERMISSIONS);
        }

        switch (file.type)
        {
        case fd_type::pipe_read:
        case fd_type::pipe_write:
            return anonymous_status(POSIX_S_IFIFO | PIPE_PERMISSIONS);
        case fd_type::socket:
            return anonymous_status(POSIX_S_IFSOCK | SOCKET_PERMISSIONS);
        case fd_type::memory_file: {
            auto status = anonymous_status(POSIX_S_IFREG | MEMORY_FILE_PERMISSIONS);
            status.size = static_cast<int64_t>(file.memory_file ? file.memory_file->content.size() : 0);
            return status;
        }
        // Linux backs eventfd and epoll descriptors with an anonymous inode, which has no file type.
        case fd_type::eventfd:
        case fd_type::epoll:
            return anonymous_status(ANONYMOUS_INODE_PERMISSIONS);
        case fd_type::file:
        case fd_type::directory:
            break;
        }

        if (file.host_path.empty())
        {
            return posix_errno::ebadf;
        }

        return posix_stat_host_path(path_from_utf8(file.host_path), true);
    }
}
