#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"
#include "../linux_stat.hpp"
#include "../procfs.hpp"

#include <cstring>
#include <sys/stat.h>

using namespace linux_errno;

namespace
{
    constexpr int AT_FDCWD = -100;

    constexpr int O_RDONLY = 0;
    constexpr int O_WRONLY = 1;
    constexpr int O_RDWR = 2;
    constexpr int O_CREAT = 0100;
    constexpr int O_TRUNC = 01000;
    constexpr int O_APPEND = 02000;

    const char* translate_open_flags(int flags)
    {
        const auto access = flags & 3;

        if (flags & O_CREAT)
        {
            if (flags & O_TRUNC)
            {
                return (access == O_RDWR) ? "w+b" : "wb";
            }
            if (flags & O_APPEND)
            {
                return (access == O_RDWR) ? "a+b" : "ab";
            }
            return (access == O_RDWR) ? "w+b" : "wb";
        }

        if (flags & O_APPEND)
        {
            return (access == O_RDWR) ? "a+b" : "ab";
        }

        if (flags & O_TRUNC)
        {
            return (access == O_RDWR) ? "w+b" : "wb";
        }

        switch (access)
        {
        case O_WRONLY:
            return "wb";
        case O_RDWR:
            return "r+b";
        default:
            return "rb";
        }
    }

    void fill_linux_stat(linux_stat& ls, const std::filesystem::path& host_path)
    {
        memset(&ls, 0, sizeof(ls));

        struct stat host_stat{};
        if (stat(host_path.string().c_str(), &host_stat) != 0)
        {
            return;
        }

        ls.st_dev = static_cast<uint64_t>(host_stat.st_dev);
        ls.st_ino = static_cast<uint64_t>(host_stat.st_ino);
        ls.st_nlink = static_cast<uint64_t>(host_stat.st_nlink);
        ls.st_mode = static_cast<uint32_t>(host_stat.st_mode);
        ls.st_uid = static_cast<uint32_t>(host_stat.st_uid);
        ls.st_gid = static_cast<uint32_t>(host_stat.st_gid);
        ls.st_rdev = static_cast<uint64_t>(host_stat.st_rdev);
        ls.st_size = static_cast<int64_t>(host_stat.st_size);
        ls.st_blksize = 4096;
        ls.st_blocks = static_cast<int64_t>((host_stat.st_size + 511) / 512);

#ifdef __APPLE__
        ls.st_atime_sec = static_cast<uint64_t>(host_stat.st_atimespec.tv_sec);
        ls.st_atime_nsec = static_cast<uint64_t>(host_stat.st_atimespec.tv_nsec);
        ls.st_mtime_sec = static_cast<uint64_t>(host_stat.st_mtimespec.tv_sec);
        ls.st_mtime_nsec = static_cast<uint64_t>(host_stat.st_mtimespec.tv_nsec);
        ls.st_ctime_sec = static_cast<uint64_t>(host_stat.st_ctimespec.tv_sec);
        ls.st_ctime_nsec = static_cast<uint64_t>(host_stat.st_ctimespec.tv_nsec);
#else
        ls.st_atime_sec = static_cast<uint64_t>(host_stat.st_atim.tv_sec);
        ls.st_atime_nsec = static_cast<uint64_t>(host_stat.st_atim.tv_nsec);
        ls.st_mtime_sec = static_cast<uint64_t>(host_stat.st_mtim.tv_sec);
        ls.st_mtime_nsec = static_cast<uint64_t>(host_stat.st_mtim.tv_nsec);
        ls.st_ctime_sec = static_cast<uint64_t>(host_stat.st_ctim.tv_sec);
        ls.st_ctime_nsec = static_cast<uint64_t>(host_stat.st_ctim.tv_nsec);
#endif
    }
}

void sys_read(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
    const auto count = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    if (fd == 0)
    {
        // stdin: return 0 (EOF) for now
        write_linux_syscall_result(c, 0);
        return;
    }

    if (!fd_entry->handle)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    std::vector<uint8_t> buffer(count);
    const auto bytes_read = fread(buffer.data(), 1, count, fd_entry->handle);

    if (bytes_read > 0)
    {
        c.emu.write_memory(buf_addr, buffer.data(), bytes_read);
    }

    write_linux_syscall_result(c, static_cast<int64_t>(bytes_read));
}

void sys_write(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
    const auto count = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    std::vector<uint8_t> buffer(count);
    c.emu.read_memory(buf_addr, buffer.data(), count);

    if (fd == 1 || fd == 2)
    {
        // stdout / stderr: write directly to host
        auto* target = (fd == 1) ? stdout : stderr;
        const auto written = fwrite(buffer.data(), 1, count, target);
        fflush(target);
        write_linux_syscall_result(c, static_cast<int64_t>(written));
        return;
    }

    if (!fd_entry->handle)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    const auto written = fwrite(buffer.data(), 1, count, fd_entry->handle);
    write_linux_syscall_result(c, static_cast<int64_t>(written));
}

void sys_open(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    // mode is arg 2, unused for now

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs paths
    if (procfs::is_procfs_path(guest_path))
    {
        auto* handle = c.emu_ref.proc_fs.open_procfs_file(c.emu_ref, guest_path);
        if (!handle)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        linux_fd new_fd{};
        new_fd.type = fd_type::file;
        new_fd.host_path = guest_path;
        new_fd.handle = handle;
        new_fd.flags = flags;

        const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
        write_linux_syscall_result(c, fd_num);
        return;
    }

    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    auto* handle = fopen(host_path.string().c_str(), translate_open_flags(flags));
    if (!handle)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    linux_fd new_fd{};
    new_fd.type = fd_type::file;
    new_fd.host_path = host_path.string();
    new_fd.handle = handle;
    new_fd.flags = flags;

    const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
    write_linux_syscall_result(c, fd_num);
}

void sys_close(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    if (!c.proc.fds.close(fd))
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_fstat(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    linux_stat ls{};

    if (!fd_entry->host_path.empty())
    {
        fill_linux_stat(ls, fd_entry->host_path);
    }
    else
    {
        // For stdin/stdout/stderr, return a character device
        ls.st_mode = 0020666; // S_IFCHR | 0666
        ls.st_rdev = 0x0501;  // /dev/pts/1
    }

    c.emu.write_memory(buf_addr, &ls, sizeof(ls));
    write_linux_syscall_result(c, 0);
}

void sys_lseek(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto offset = static_cast<int64_t>(get_linux_syscall_argument(c.emu, 1));
    const auto whence = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry || !fd_entry->handle)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    int origin = SEEK_SET;
    if (whence == 1)
    {
        origin = SEEK_CUR;
    }
    else if (whence == 2)
    {
        origin = SEEK_END;
    }

    if (fseek(fd_entry->handle, static_cast<long>(offset), origin) != 0)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    const auto pos = ftell(fd_entry->handle);
    write_linux_syscall_result(c, pos);
}

void sys_access(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    // mode is arg 1, unused for now

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs paths — they always "exist"
    if (procfs::is_procfs_path(guest_path) || procfs::is_procfs_symlink(guest_path))
    {
        write_linux_syscall_result(c, 0);
        return;
    }

    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    if (std::filesystem::exists(host_path))
    {
        write_linux_syscall_result(c, 0);
    }
    else
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
    }
}

void sys_openat(const linux_syscall_context& c)
{
    const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto path_addr = get_linux_syscall_argument(c.emu, 1);
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
    // mode is arg 3

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs paths
    if (procfs::is_procfs_path(guest_path))
    {
        auto* handle = c.emu_ref.proc_fs.open_procfs_file(c.emu_ref, guest_path);
        if (!handle)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        linux_fd new_fd{};
        new_fd.type = fd_type::file;
        new_fd.host_path = guest_path;
        new_fd.handle = handle;
        new_fd.flags = flags;

        const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
        write_linux_syscall_result(c, fd_num);
        return;
    }

    // AT_FDCWD means use cwd (which in emulation is just the root)
    std::filesystem::path host_path;
    if (dirfd == AT_FDCWD || (!guest_path.empty() && guest_path[0] == '/'))
    {
        host_path = c.emu_ref.file_sys.translate(guest_path);
    }
    else
    {
        // Relative to dirfd — not yet supported, treat as relative to root
        host_path = c.emu_ref.file_sys.translate(guest_path);
    }

    auto* handle = fopen(host_path.string().c_str(), translate_open_flags(flags));
    if (!handle)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    linux_fd new_fd{};
    new_fd.type = fd_type::file;
    new_fd.host_path = host_path.string();
    new_fd.handle = handle;
    new_fd.flags = flags;

    const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
    write_linux_syscall_result(c, fd_num);
}

// --- Phase 4b: Additional file syscalls ---

void sys_stat(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs paths
    if (procfs::is_procfs_path(guest_path))
    {
        linux_stat ls{};
        if (c.emu_ref.proc_fs.stat_procfs(c.emu_ref, guest_path, ls))
        {
            c.emu.write_memory(buf_addr, &ls, sizeof(ls));
            write_linux_syscall_result(c, 0);
        }
        else
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
        }
        return;
    }

    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    if (!std::filesystem::exists(host_path))
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    linux_stat ls{};
    fill_linux_stat(ls, host_path);

    c.emu.write_memory(buf_addr, &ls, sizeof(ls));
    write_linux_syscall_result(c, 0);
}

void sys_lstat(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs paths (important for symlinks like /proc/self/exe)
    if (procfs::is_procfs_path(guest_path) || procfs::is_procfs_symlink(guest_path))
    {
        linux_stat ls{};
        if (c.emu_ref.proc_fs.stat_procfs(c.emu_ref, guest_path, ls))
        {
            c.emu.write_memory(buf_addr, &ls, sizeof(ls));
            write_linux_syscall_result(c, 0);
        }
        else
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
        }
        return;
    }

    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    struct stat host_stat{};
    if (lstat(host_path.string().c_str(), &host_stat) != 0)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    linux_stat ls{};
    fill_linux_stat(ls, host_path);

    c.emu.write_memory(buf_addr, &ls, sizeof(ls));
    write_linux_syscall_result(c, 0);
}

void sys_pread64(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
    const auto count = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));
    const auto offset = static_cast<int64_t>(get_linux_syscall_argument(c.emu, 3));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry || !fd_entry->handle)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    const auto saved_pos = ftell(fd_entry->handle);
    fseek(fd_entry->handle, static_cast<long>(offset), SEEK_SET);

    std::vector<uint8_t> buffer(count);
    const auto bytes_read = fread(buffer.data(), 1, count, fd_entry->handle);

    fseek(fd_entry->handle, saved_pos, SEEK_SET);

    if (bytes_read > 0)
    {
        c.emu.write_memory(buf_addr, buffer.data(), bytes_read);
    }

    write_linux_syscall_result(c, static_cast<int64_t>(bytes_read));
}

void sys_writev(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto iov_addr = get_linux_syscall_argument(c.emu, 1);
    const auto iovcnt = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    int64_t total_written = 0;

    for (int i = 0; i < iovcnt; ++i)
    {
        const auto entry_addr = iov_addr + static_cast<uint64_t>(i) * 16;
        uint64_t iov_base{};
        uint64_t iov_len{};
        c.emu.read_memory(entry_addr, &iov_base, 8);
        c.emu.read_memory(entry_addr + 8, &iov_len, 8);

        if (iov_len == 0)
        {
            continue;
        }

        std::vector<uint8_t> buffer(iov_len);
        c.emu.read_memory(iov_base, buffer.data(), iov_len);

        if (fd == 1 || fd == 2)
        {
            auto* target = (fd == 1) ? stdout : stderr;
            const auto written = fwrite(buffer.data(), 1, iov_len, target);
            fflush(target);
            total_written += static_cast<int64_t>(written);
        }
        else if (fd_entry->handle)
        {
            const auto written = fwrite(buffer.data(), 1, iov_len, fd_entry->handle);
            total_written += static_cast<int64_t>(written);
        }
        else
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }
    }

    write_linux_syscall_result(c, total_written);
}

void sys_dup(const linux_syscall_context& c)
{
    const auto oldfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto result = c.proc.fds.dup_fd(oldfd);
    write_linux_syscall_result(c, result >= 0 ? result : -LINUX_EBADF);
}

void sys_dup2(const linux_syscall_context& c)
{
    const auto oldfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto newfd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    const auto result = c.proc.fds.dup2_fd(oldfd, newfd);
    write_linux_syscall_result(c, result >= 0 ? result : -LINUX_EBADF);
}

void sys_dup3(const linux_syscall_context& c)
{
    const auto oldfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto newfd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    if (oldfd == newfd)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    const auto result = c.proc.fds.dup2_fd(oldfd, newfd);
    if (result < 0)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    constexpr int LINUX_O_CLOEXEC = 02000000;
    if (flags & LINUX_O_CLOEXEC)
    {
        c.proc.fds.set_close_on_exec(newfd, true);
    }

    write_linux_syscall_result(c, result);
}

void sys_fcntl(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto cmd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    const auto arg = get_linux_syscall_argument(c.emu, 2);

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    constexpr int F_DUPFD = 0;
    constexpr int F_GETFD = 1;
    constexpr int F_SETFD = 2;
    constexpr int F_GETFL = 3;
    constexpr int F_SETFL = 4;
    constexpr int F_DUPFD_CLOEXEC = 1030;

    switch (cmd)
    {
    case F_DUPFD: {
        const auto new_fd = c.proc.fds.dup_fd(fd);
        write_linux_syscall_result(c, new_fd >= 0 ? new_fd : -LINUX_EMFILE);
        break;
    }
    case F_DUPFD_CLOEXEC: {
        const auto new_fd = c.proc.fds.dup_fd(fd);
        if (new_fd >= 0)
        {
            c.proc.fds.set_close_on_exec(new_fd, true);
        }
        write_linux_syscall_result(c, new_fd >= 0 ? new_fd : -LINUX_EMFILE);
        break;
    }
    case F_GETFD:
        write_linux_syscall_result(c, fd_entry->close_on_exec ? 1 : 0);
        break;
    case F_SETFD:
        fd_entry->close_on_exec = (arg & 1) != 0;
        write_linux_syscall_result(c, 0);
        break;
    case F_GETFL:
        write_linux_syscall_result(c, fd_entry->flags);
        break;
    case F_SETFL:
        fd_entry->flags = static_cast<int>(arg);
        write_linux_syscall_result(c, 0);
        break;
    default:
        write_linux_syscall_result(c, -LINUX_EINVAL);
        break;
    }
}

void sys_getcwd(const linux_syscall_context& c)
{
    const auto buf_addr = get_linux_syscall_argument(c.emu, 0);
    const auto size = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));

    const char* cwd = "/";
    const auto cwd_len = strlen(cwd) + 1;

    if (size < cwd_len)
    {
        write_linux_syscall_result(c, -LINUX_ERANGE);
        return;
    }

    c.emu.write_memory(buf_addr, cwd, cwd_len);
    write_linux_syscall_result(c, static_cast<int64_t>(buf_addr));
}

void sys_readlink(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
    const auto bufsiz = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs symlinks
    if (procfs::is_procfs_symlink(guest_path))
    {
        auto target = c.emu_ref.proc_fs.resolve_symlink(c.emu_ref, guest_path);
        if (!target)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }
        const auto write_len = std::min(bufsiz, target->size());
        c.emu.write_memory(buf_addr, target->data(), write_len);
        write_linux_syscall_result(c, static_cast<int64_t>(write_len));
        return;
    }

    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    const auto target = std::filesystem::read_symlink(host_path, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    const auto target_str = target.string();
    const auto write_len = std::min(bufsiz, target_str.size());
    c.emu.write_memory(buf_addr, target_str.data(), write_len);
    write_linux_syscall_result(c, static_cast<int64_t>(write_len));
}

void sys_readlinkat(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 1);
    const auto buf_addr = get_linux_syscall_argument(c.emu, 2);
    const auto bufsiz = static_cast<size_t>(get_linux_syscall_argument(c.emu, 3));

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs symlinks
    if (procfs::is_procfs_symlink(guest_path))
    {
        auto target = c.emu_ref.proc_fs.resolve_symlink(c.emu_ref, guest_path);
        if (!target)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }
        const auto write_len = std::min(bufsiz, target->size());
        c.emu.write_memory(buf_addr, target->data(), write_len);
        write_linux_syscall_result(c, static_cast<int64_t>(write_len));
        return;
    }

    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    const auto target = std::filesystem::read_symlink(host_path, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    const auto target_str = target.string();
    const auto write_len = std::min(bufsiz, target_str.size());
    c.emu.write_memory(buf_addr, target_str.data(), write_len);
    write_linux_syscall_result(c, static_cast<int64_t>(write_len));
}

void sys_getdents64(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    if (!c.proc.fds.get(fd))
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    // Stub: return 0 (end of directory)
    write_linux_syscall_result(c, 0);
}

void sys_newfstatat(const linux_syscall_context& c)
{
    const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto path_addr = get_linux_syscall_argument(c.emu, 1);
    const auto buf_addr = get_linux_syscall_argument(c.emu, 2);
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 3));

    const auto guest_path = read_string<char>(c.emu, path_addr);

    constexpr int AT_EMPTY_PATH = 0x1000;
    if (guest_path.empty() && (flags & AT_EMPTY_PATH))
    {
        auto* fd_entry = c.proc.fds.get(dirfd);
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        linux_stat ls{};
        if (!fd_entry->host_path.empty())
        {
            fill_linux_stat(ls, fd_entry->host_path);
        }
        else
        {
            ls.st_mode = 0020666;
            ls.st_rdev = 0x0501;
        }

        c.emu.write_memory(buf_addr, &ls, sizeof(ls));
        write_linux_syscall_result(c, 0);
        return;
    }

    // Intercept procfs paths
    if (procfs::is_procfs_path(guest_path))
    {
        linux_stat ls{};
        if (c.emu_ref.proc_fs.stat_procfs(c.emu_ref, guest_path, ls))
        {
            c.emu.write_memory(buf_addr, &ls, sizeof(ls));
            write_linux_syscall_result(c, 0);
        }
        else
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
        }
        return;
    }

    std::filesystem::path host_path;
    if (dirfd == AT_FDCWD || (!guest_path.empty() && guest_path[0] == '/'))
    {
        host_path = c.emu_ref.file_sys.translate(guest_path);
    }
    else
    {
        host_path = c.emu_ref.file_sys.translate(guest_path);
    }

    if (!std::filesystem::exists(host_path))
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    linux_stat ls{};
    fill_linux_stat(ls, host_path);

    c.emu.write_memory(buf_addr, &ls, sizeof(ls));
    write_linux_syscall_result(c, 0);
}

// --- Phase 4c: Additional file syscalls ---

void sys_rename(const linux_syscall_context& c)
{
    const auto oldpath_addr = get_linux_syscall_argument(c.emu, 0);
    const auto newpath_addr = get_linux_syscall_argument(c.emu, 1);

    const auto old_guest = read_string<char>(c.emu, oldpath_addr);
    const auto new_guest = read_string<char>(c.emu, newpath_addr);

    const auto old_host = c.emu_ref.file_sys.translate(old_guest);
    const auto new_host = c.emu_ref.file_sys.translate(new_guest);

    std::error_code ec{};
    std::filesystem::rename(old_host, new_host, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_renameat2(const linux_syscall_context& c)
{
    // renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags)
    // const auto olddirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto oldpath_addr = get_linux_syscall_argument(c.emu, 1);
    // const auto newdirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
    const auto newpath_addr = get_linux_syscall_argument(c.emu, 3);
    // const auto flags = static_cast<unsigned int>(get_linux_syscall_argument(c.emu, 4));

    const auto old_guest = read_string<char>(c.emu, oldpath_addr);
    const auto new_guest = read_string<char>(c.emu, newpath_addr);

    const auto old_host = c.emu_ref.file_sys.translate(old_guest);
    const auto new_host = c.emu_ref.file_sys.translate(new_guest);

    std::error_code ec{};
    std::filesystem::rename(old_host, new_host, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_unlink(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    if (!std::filesystem::remove(host_path, ec))
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_unlinkat(const linux_syscall_context& c)
{
    // unlinkat(int dirfd, const char *pathname, int flags)
    // const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto path_addr = get_linux_syscall_argument(c.emu, 1);
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    constexpr int AT_REMOVEDIR = 0x200;

    std::error_code ec{};
    if (flags & AT_REMOVEDIR)
    {
        if (!std::filesystem::remove(host_path, ec))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }
    }
    else
    {
        if (!std::filesystem::remove(host_path, ec))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }
    }

    write_linux_syscall_result(c, 0);
}

void sys_mkdir(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    // mode is arg 1

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    if (!std::filesystem::create_directory(host_path, ec))
    {
        if (ec)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }
        // Directory already exists
        write_linux_syscall_result(c, -LINUX_EEXIST);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_mkdirat(const linux_syscall_context& c)
{
    // mkdirat(int dirfd, const char *pathname, mode_t mode)
    // const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto path_addr = get_linux_syscall_argument(c.emu, 1);
    // mode is arg 2

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    if (!std::filesystem::create_directory(host_path, ec))
    {
        if (ec)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }
        write_linux_syscall_result(c, -LINUX_EEXIST);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_rmdir(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    if (!std::filesystem::remove(host_path, ec))
    {
        write_linux_syscall_result(c, ec ? -LINUX_ENOENT : -LINUX_ENOTEMPTY);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_symlink(const linux_syscall_context& c)
{
    const auto target_addr = get_linux_syscall_argument(c.emu, 0);
    const auto linkpath_addr = get_linux_syscall_argument(c.emu, 1);

    const auto target = read_string<char>(c.emu, target_addr);
    const auto linkpath_guest = read_string<char>(c.emu, linkpath_addr);
    const auto linkpath_host = c.emu_ref.file_sys.translate(linkpath_guest);

    std::error_code ec{};
    std::filesystem::create_symlink(target, linkpath_host, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_symlinkat(const linux_syscall_context& c)
{
    // symlinkat(const char *target, int newdirfd, const char *linkpath)
    const auto target_addr = get_linux_syscall_argument(c.emu, 0);
    // const auto newdirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    const auto linkpath_addr = get_linux_syscall_argument(c.emu, 2);

    const auto target = read_string<char>(c.emu, target_addr);
    const auto linkpath_guest = read_string<char>(c.emu, linkpath_addr);
    const auto linkpath_host = c.emu_ref.file_sys.translate(linkpath_guest);

    std::error_code ec{};
    std::filesystem::create_symlink(target, linkpath_host, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_chmod(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    const auto mode = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 1));

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    std::filesystem::permissions(host_path, static_cast<std::filesystem::perms>(mode), std::filesystem::perm_options::replace, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_fchmod(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto mode = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 1));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    if (fd_entry->host_path.empty())
    {
        // Can't chmod special fds
        write_linux_syscall_result(c, 0);
        return;
    }

    std::error_code ec{};
    std::filesystem::permissions(fd_entry->host_path, static_cast<std::filesystem::perms>(mode), std::filesystem::perm_options::replace,
                                 ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_fchmodat(const linux_syscall_context& c)
{
    // fchmodat(int dirfd, const char *pathname, mode_t mode, int flags)
    // const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto path_addr = get_linux_syscall_argument(c.emu, 1);
    const auto mode = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 2));
    // flags is arg 3

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    std::filesystem::permissions(host_path, static_cast<std::filesystem::perms>(mode), std::filesystem::perm_options::replace, ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_chown(const linux_syscall_context& c)
{
    // chown is a no-op in emulation — we don't change real ownership
    (void)c;
    write_linux_syscall_result(c, 0);
}

void sys_fchown(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    if (!c.proc.fds.get(fd))
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    // No-op in emulation
    write_linux_syscall_result(c, 0);
}

void sys_fchownat(const linux_syscall_context& c)
{
    // No-op in emulation
    (void)c;
    write_linux_syscall_result(c, 0);
}

void sys_truncate(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    const auto length = static_cast<int64_t>(get_linux_syscall_argument(c.emu, 1));

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    std::error_code ec{};
    std::filesystem::resize_file(host_path, static_cast<uintmax_t>(length), ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_ftruncate(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto length = static_cast<int64_t>(get_linux_syscall_argument(c.emu, 1));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry || !fd_entry->handle)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    if (fd_entry->host_path.empty())
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    std::error_code ec{};
    std::filesystem::resize_file(fd_entry->host_path, static_cast<uintmax_t>(length), ec);
    if (ec)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    write_linux_syscall_result(c, 0);
}

void sys_pwrite64(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
    const auto count = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));
    const auto offset = static_cast<int64_t>(get_linux_syscall_argument(c.emu, 3));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry || !fd_entry->handle)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    const auto saved_pos = ftell(fd_entry->handle);
    fseek(fd_entry->handle, static_cast<long>(offset), SEEK_SET);

    std::vector<uint8_t> buffer(count);
    c.emu.read_memory(buf_addr, buffer.data(), count);
    const auto bytes_written = fwrite(buffer.data(), 1, count, fd_entry->handle);

    fseek(fd_entry->handle, saved_pos, SEEK_SET);

    write_linux_syscall_result(c, static_cast<int64_t>(bytes_written));
}

void sys_readv(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto iov_addr = get_linux_syscall_argument(c.emu, 1);
    const auto iovcnt = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    int64_t total_read = 0;

    for (int i = 0; i < iovcnt; ++i)
    {
        const auto entry_addr = iov_addr + static_cast<uint64_t>(i) * 16;
        uint64_t iov_base{};
        uint64_t iov_len{};
        c.emu.read_memory(entry_addr, &iov_base, 8);
        c.emu.read_memory(entry_addr + 8, &iov_len, 8);

        if (iov_len == 0)
        {
            continue;
        }

        if (fd == 0)
        {
            // stdin: EOF
            break;
        }

        if (!fd_entry->handle)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        std::vector<uint8_t> buffer(iov_len);
        const auto bytes_read = fread(buffer.data(), 1, iov_len, fd_entry->handle);

        if (bytes_read > 0)
        {
            c.emu.write_memory(iov_base, buffer.data(), bytes_read);
            total_read += static_cast<int64_t>(bytes_read);
        }

        if (bytes_read < iov_len)
        {
            break; // Short read — don't continue to next iov
        }
    }

    write_linux_syscall_result(c, total_read);
}

void sys_faccessat(const linux_syscall_context& c)
{
    // faccessat(int dirfd, const char *pathname, int mode, int flags)
    // const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto path_addr = get_linux_syscall_argument(c.emu, 1);
    // mode is arg 2, flags is arg 3

    const auto guest_path = read_string<char>(c.emu, path_addr);

    // Intercept procfs paths
    if (procfs::is_procfs_path(guest_path) || procfs::is_procfs_symlink(guest_path))
    {
        write_linux_syscall_result(c, 0);
        return;
    }

    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    if (std::filesystem::exists(host_path))
    {
        write_linux_syscall_result(c, 0);
    }
    else
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
    }
}

void sys_statfs(const linux_syscall_context& c)
{
    const auto path_addr = get_linux_syscall_argument(c.emu, 0);
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);

    const auto guest_path = read_string<char>(c.emu, path_addr);
    const auto host_path = c.emu_ref.file_sys.translate(guest_path);

    if (!std::filesystem::exists(host_path))
    {
        write_linux_syscall_result(c, -LINUX_ENOENT);
        return;
    }

    // Linux struct statfs (120 bytes on x86-64)
#pragma pack(push, 1)
    struct linux_statfs
    {
        int64_t f_type;
        int64_t f_bsize;
        int64_t f_blocks;
        int64_t f_bfree;
        int64_t f_bavail;
        int64_t f_files;
        int64_t f_ffree;
        int64_t f_fsid[2]; // actually __kernel_fsid_t (2 ints but padded to 2 longs)
        int64_t f_namelen;
        int64_t f_frsize;
        int64_t f_flags;
        int64_t f_spare[4];
    };
#pragma pack(pop)

    linux_statfs sfs{};
    sfs.f_type = 0xEF53; // EXT4_SUPER_MAGIC
    sfs.f_bsize = 4096;
    sfs.f_blocks = 1000000;
    sfs.f_bfree = 500000;
    sfs.f_bavail = 450000;
    sfs.f_files = 100000;
    sfs.f_ffree = 50000;
    sfs.f_namelen = 255;
    sfs.f_frsize = 4096;

    c.emu.write_memory(buf_addr, &sfs, sizeof(sfs));
    write_linux_syscall_result(c, 0);
}

void sys_fstatfs(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto buf_addr = get_linux_syscall_argument(c.emu, 1);

    if (!c.proc.fds.get(fd))
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

#pragma pack(push, 1)
    struct linux_statfs
    {
        int64_t f_type;
        int64_t f_bsize;
        int64_t f_blocks;
        int64_t f_bfree;
        int64_t f_bavail;
        int64_t f_files;
        int64_t f_ffree;
        int64_t f_fsid[2];
        int64_t f_namelen;
        int64_t f_frsize;
        int64_t f_flags;
        int64_t f_spare[4];
    };
#pragma pack(pop)

    linux_statfs sfs{};
    sfs.f_type = 0xEF53;
    sfs.f_bsize = 4096;
    sfs.f_blocks = 1000000;
    sfs.f_bfree = 500000;
    sfs.f_bavail = 450000;
    sfs.f_files = 100000;
    sfs.f_ffree = 50000;
    sfs.f_namelen = 255;
    sfs.f_frsize = 4096;

    c.emu.write_memory(buf_addr, &sfs, sizeof(sfs));
    write_linux_syscall_result(c, 0);
}

void sys_ioctl(const linux_syscall_context& c)
{
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto request = get_linux_syscall_argument(c.emu, 1);

    constexpr uint64_t TCGETS = 0x5401;
    constexpr uint64_t TCSETS = 0x5402;
    constexpr uint64_t TCSETSW = 0x5403;
    constexpr uint64_t TCSETSF = 0x5404;
    constexpr uint64_t TIOCGWINSZ = 0x5413;
    constexpr uint64_t TIOCSWINSZ = 0x5414;
    constexpr uint64_t TIOCGPGRP = 0x540F;
    constexpr uint64_t FIONREAD = 0x541B;

    auto* fd_entry = c.proc.fds.get(fd);
    if (!fd_entry)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    switch (request)
    {
    case TCGETS:
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TIOCGPGRP:
        // Not a terminal in emulation
        write_linux_syscall_result(c, -LINUX_ENOTTY);
        break;

    case TIOCGWINSZ: {
        const auto arg_addr = get_linux_syscall_argument(c.emu, 2);
#pragma pack(push, 1)
        struct winsize
        {
            uint16_t ws_row;
            uint16_t ws_col;
            uint16_t ws_xpixel;
            uint16_t ws_ypixel;
        };
#pragma pack(pop)
        winsize ws{};
        ws.ws_row = 25;
        ws.ws_col = 80;
        c.emu.write_memory(arg_addr, &ws, sizeof(ws));
        write_linux_syscall_result(c, 0);
        break;
    }

    case TIOCSWINSZ:
        write_linux_syscall_result(c, 0);
        break;

    case FIONREAD: {
        const auto arg_addr = get_linux_syscall_argument(c.emu, 2);
        int32_t count = 0;
        c.emu.write_memory(arg_addr, &count, sizeof(count));
        write_linux_syscall_result(c, 0);
        break;
    }

    default:
        write_linux_syscall_result(c, -LINUX_ENOTTY);
        break;
    }
}
