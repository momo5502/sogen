#pragma once

#include "posix_fd_table.hpp"
#include "posix_file_system.hpp"
#include "posix_result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>

namespace sogen
{
    // Linux and XNU share the S_IF* file type values and the permission bits of st_mode.
    constexpr uint32_t POSIX_S_IFMT = 0170000;
    constexpr uint32_t POSIX_S_IFIFO = 0010000;
    constexpr uint32_t POSIX_S_IFCHR = 0020000;
    constexpr uint32_t POSIX_S_IFDIR = 0040000;
    constexpr uint32_t POSIX_S_IFREG = 0100000;
    constexpr uint32_t POSIX_S_IFLNK = 0120000;
    constexpr uint32_t POSIX_S_IFSOCK = 0140000;
    constexpr uint32_t POSIX_PERMISSION_BITS = 07777;

    struct posix_timespec
    {
        int64_t seconds{};
        int64_t nanoseconds{};
    };

    struct posix_file_status
    {
        uint32_t mode{};
        uint64_t device{};
        uint64_t inode{};
        uint64_t link_count{};
        uint32_t user_id{};
        uint32_t group_id{};
        uint64_t special_device{};
        int64_t size{};
        posix_timespec access_time{};
        posix_timespec modification_time{};
        posix_timespec change_time{};

        uint32_t file_type() const
        {
            return this->mode & POSIX_S_IFMT;
        }
    };

    using posix_stat_result = std::variant<posix_file_status, posix_errno>;

    posix_stat_result posix_stat_host_path(const std::filesystem::path& host_path, bool follow_symlinks);
    posix_stat_result posix_stat_path(const posix_file_system& file_system, const std::string& guest_path, bool follow_symlinks);
    posix_stat_result posix_stat_descriptor(const posix_fd& file);
}
