#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace sogen
{
    namespace
    {
        struct macos_timespec
        {
            int64_t seconds;
            int64_t nanoseconds;
        };

        // struct stat with 64-bit inodes, the only layout x86-64 macOS uses.
        struct macos_stat64
        {
            int32_t st_dev;
            uint16_t st_mode;
            uint16_t st_nlink;
            uint64_t st_ino;
            uint32_t st_uid;
            uint32_t st_gid;
            int32_t st_rdev;
            int32_t padding;
            macos_timespec st_atimespec;
            macos_timespec st_mtimespec;
            macos_timespec st_ctimespec;
            macos_timespec st_birthtimespec;
            int64_t st_size;
            int64_t st_blocks;
            int32_t st_blksize;
            uint32_t st_flags;
            uint32_t st_gen;
            int32_t st_lspare;
            std::array<int64_t, 2> st_qspare;
        };

        static_assert(sizeof(macos_stat64) == 144);
        static_assert(offsetof(macos_stat64, st_ino) == 8);
        static_assert(offsetof(macos_stat64, st_atimespec) == 32);
        static_assert(offsetof(macos_stat64, st_size) == 96);
        static_assert(offsetof(macos_stat64, st_blksize) == 112);

        constexpr uint16_t MACOS_S_IFIFO = 0010000;
        constexpr uint16_t MACOS_S_IFDIR = 0040000;
        constexpr uint16_t MACOS_S_IFREG = 0100000;
        constexpr uint16_t MACOS_S_IFLNK = 0120000;
        constexpr uint16_t PIPE_PERMISSIONS = 0600;
        constexpr int32_t BLOCK_SIZE = 4096;
        constexpr int64_t STAT_BLOCK_UNIT = 512;

        constexpr macos_timespec FILE_TIME{.seconds = MACOS_EPOCH_SECONDS, .nanoseconds = 0};

        std::optional<macos_stat64> stat_host_path(macos_emulator& emu, const std::string& guest_path, const bool follow_symlinks)
        {
            const auto host_path = emu.file_system.translate(guest_path);

            std::error_code ec{};
            const auto status = follow_symlinks ? std::filesystem::status(host_path, ec) : std::filesystem::symlink_status(host_path, ec);
            if (ec || !std::filesystem::exists(status))
            {
                return std::nullopt;
            }

            uint16_t type{};
            int64_t size{};
            switch (status.type())
            {
            case std::filesystem::file_type::regular:
                type = MACOS_S_IFREG;
                size = static_cast<int64_t>(std::filesystem::file_size(host_path, ec));
                break;
            case std::filesystem::file_type::directory:
                type = MACOS_S_IFDIR;
                break;
            case std::filesystem::file_type::symlink:
                type = MACOS_S_IFLNK;
                break;
            default:
                return std::nullopt;
            }

            const auto id = emu.file_ids.id_for(guest_path);

            macos_stat64 result{};
            result.st_dev = id.fsid[0];
            result.st_mode = static_cast<uint16_t>(type | (static_cast<uint16_t>(status.permissions()) & 07777));
            result.st_nlink = 1;
            result.st_ino = id.object_id;
            result.st_atimespec = FILE_TIME;
            result.st_mtimespec = FILE_TIME;
            result.st_ctimespec = FILE_TIME;
            result.st_birthtimespec = FILE_TIME;
            result.st_size = size;
            result.st_blocks = (size + STAT_BLOCK_UNIT - 1) / STAT_BLOCK_UNIT;
            result.st_blksize = BLOCK_SIZE;
            return result;
        }

        void write_stat_result(const macos_syscall_context& c, const std::optional<macos_stat64>& result, const uint64_t buffer)
        {
            if (!result)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
                return;
            }

            if (!c.emu.try_write_memory(buffer, &*result, sizeof(*result)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
        }

        void stat_path(const macos_syscall_context& c, const bool follow_symlinks)
        {
            const auto guest_path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
            if (!guest_path)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            const auto result = stat_host_path(c.emu_ref, *guest_path, follow_symlinks);
            c.emu_ref.log.log("%sstat64(%s)%s\n", follow_symlinks ? "" : "l", guest_path->c_str(), result ? "" : " -> ENOENT");
            write_stat_result(c, result, get_macos_syscall_argument(c.emu, 1));
        }
    }

    void sys_stat64(const macos_syscall_context& c)
    {
        stat_path(c, true);
    }

    void sys_lstat64(const macos_syscall_context& c)
    {
        stat_path(c, false);
    }

    void sys_fstat64(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto buffer = get_macos_syscall_argument(c.emu, 1);

        const auto* file = c.emu_ref.fds.get(fd);
        if (file == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        // The standard streams are presented as pipes, as when output is captured, so runs do not depend on whether
        // the host has a terminal.
        if (file->is_standard_stream())
        {
            macos_stat64 pipe{};
            pipe.st_mode = MACOS_S_IFIFO | PIPE_PERMISSIONS;
            pipe.st_nlink = 1;
            pipe.st_blksize = BLOCK_SIZE;
            write_stat_result(c, pipe, buffer);
            return;
        }

        write_stat_result(c, stat_host_path(c.emu_ref, file->guest_path, true), buffer);
    }

    void sys_access(const macos_syscall_context& c)
    {
        constexpr uint64_t x_ok = 0x1;
        constexpr uint64_t w_ok = 0x2;

        const auto guest_path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        const auto mode = get_macos_syscall_argument(c.emu, 1);
        if (!guest_path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        const auto result = stat_host_path(c.emu_ref, *guest_path, true);
        c.emu_ref.log.log("access(%s, %llu)%s\n", guest_path->c_str(), static_cast<unsigned long long>(mode), result ? "" : " -> ENOENT");

        if (!result)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        if (mode & w_ok)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EROFS);
            return;
        }

        if ((mode & x_ok) && (result->st_mode & 0111) == 0)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EACCES);
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}
