#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <posix_path_syscalls.hpp>
#include <posix_stat.hpp>

#include <array>
#include <cstddef>
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

        constexpr int32_t BLOCK_SIZE = 4096;
        constexpr int64_t STAT_BLOCK_UNIT = 512;
        constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x20;

        constexpr macos_timespec FILE_TIME{.seconds = MACOS_EPOCH_SECONDS, .nanoseconds = 0};

        // Only the file type, permissions and the size of regular files come from the host. Identifiers and times
        // are synthetic, so runs are deterministic and never expose host inode numbers.
        macos_stat64 to_macos_stat(macos_emulator& emu, const posix_file_status& status, const std::string& guest_path)
        {
            const auto id = emu.file_ids.id_for(guest_path);
            const auto size = status.file_type() == POSIX_S_IFREG ? status.size : 0;

            macos_stat64 result{};
            result.st_dev = id.fsid[0];
            result.st_mode = static_cast<uint16_t>(status.mode);
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

        // Pipes and sockets have no path to name them by.
        macos_stat64 to_anonymous_macos_stat(const posix_file_status& status)
        {
            macos_stat64 result{};
            result.st_mode = static_cast<uint16_t>(status.mode);
            result.st_nlink = 1;
            result.st_blksize = BLOCK_SIZE;
            return result;
        }

        void write_stat(const macos_syscall_context& c, const macos_stat64& result, const uint64_t buffer)
        {
            if (!c.emu.try_write_memory(buffer, &result, sizeof(result)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
        }

        void stat_at(const macos_syscall_context& c, const int directory_fd, const size_t path_index, const uint64_t buffer,
                     const bool follow_symlinks)
        {
            const auto guest_path = read_guest_path_argument(c, path_index, directory_fd);
            if (const auto* error = std::get_if<posix_errno>(&guest_path))
            {
                write_macos_syscall_result(c, *error);
                return;
            }

            const auto& path = std::get<std::string>(guest_path);
            const auto status = posix_stat_path(c.emu_ref.file_system, path, follow_symlinks);
            const auto* file_status = std::get_if<posix_file_status>(&status);
            c.emu_ref.log.log("%sstat64(%s)%s\n", follow_symlinks ? "" : "l", path.c_str(), file_status ? "" : " -> error");
            if (!file_status)
            {
                write_macos_syscall_result(c, std::get<posix_errno>(status));
                return;
            }

            write_stat(c, to_macos_stat(c.emu_ref, *file_status, path), buffer);
        }
    }

    void sys_stat64(const macos_syscall_context& c)
    {
        stat_at(c, MACOS_AT_FDCWD, 0, get_macos_syscall_argument(c.emu, 1), true);
    }

    void sys_lstat64(const macos_syscall_context& c)
    {
        stat_at(c, MACOS_AT_FDCWD, 0, get_macos_syscall_argument(c.emu, 1), false);
    }

    void sys_fstatat64(const macos_syscall_context& c)
    {
        const auto directory_fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto flags = get_macos_syscall_argument(c.emu, 3);
        stat_at(c, directory_fd, 1, get_macos_syscall_argument(c.emu, 2), (flags & AT_SYMLINK_NOFOLLOW) == 0);
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

        const auto status = posix_stat_descriptor(*file);
        if (const auto* error = std::get_if<posix_errno>(&status))
        {
            write_macos_syscall_result(c, *error);
            return;
        }

        const auto& file_status = std::get<posix_file_status>(status);
        write_stat(c, is_vnode(*file) ? to_macos_stat(c.emu_ref, file_status, file->guest_path) : to_anonymous_macos_stat(file_status),
                   buffer);
    }

    void sys_access(const macos_syscall_context& c)
    {
        const auto guest_path = read_guest_path_argument(c, 0);
        if (const auto* error = std::get_if<posix_errno>(&guest_path))
        {
            write_macos_syscall_result(c, *error);
            return;
        }

        const auto& path = std::get<std::string>(guest_path);
        const auto mode = get_macos_syscall_argument(c.emu, 1);
        const auto result = posix_access(c.emu_ref.file_system, path, mode);
        c.emu_ref.log.log("access(%s, %llu)%s\n", path.c_str(), static_cast<unsigned long long>(mode),
                          std::holds_alternative<posix_errno>(result) ? " -> error" : "");
        write_macos_syscall_result(c, result);
    }
}
