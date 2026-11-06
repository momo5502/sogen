#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"
#include "../linux_stat.hpp"
#include "../procfs.hpp"

#include <posix_descriptor_syscalls.hpp>
#include <posix_io_syscalls.hpp>
#include <posix_open.hpp>
#include <posix_path_resolution.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sogen
{

    using namespace linux_errno; // NOLINT(google-build-using-namespace)

    void cleanup_linux_socket_state(int fd);
    bool linux_socket_has_host_socket(const posix_fd& fd_entry);
    void linux_socket_apply_fd_flags(int fd, const posix_fd& fd_entry);
    int64_t linux_socket_send_guest_buffer(const linux_syscall_context& c, int fd, uint64_t buf_addr, size_t count, int flags);
    int64_t linux_socket_recv_guest_buffer(const linux_syscall_context& c, int fd, uint64_t buf_addr, size_t count, int flags);
    int64_t linux_socket_writev_from_guest(const linux_syscall_context& c, int fd, uint64_t iov_addr, int iovcnt);
    int64_t linux_socket_readv_to_guest(const linux_syscall_context& c, int fd, uint64_t iov_addr, int iovcnt);

#ifdef _WIN32
#define lstat stat
#endif

    namespace
    {
        constexpr int LINUX_AT_FDCWD = -100;

        constexpr int LINUX_O_WRONLY = 1;
        constexpr int LINUX_O_RDWR = 2;
        constexpr int LINUX_O_CREAT = 0100;
        constexpr int LINUX_O_EXCL = 0200;
        constexpr int LINUX_O_TRUNC = 01000;
        constexpr int LINUX_O_APPEND = 02000;
        constexpr int LINUX_O_DIRECTORY = 0200000;
        constexpr int LINUX_O_NONBLOCK = 04000;
        constexpr int LINUX_O_CLOEXEC = 02000000;

        // getrlimit reports RLIMIT_NOFILE as unlimited, so only the descriptor's int type bounds it.
        constexpr int LINUX_DESCRIPTOR_LIMIT = std::numeric_limits<int>::max();

        posix_io_context io_context(const linux_syscall_context& c)
        {
            return {
                .fds = c.proc.fds,
                .memory = c.emu_ref.memory,
                .on_stdout = c.emu_ref.on_stdout,
                .on_stderr = c.emu_ref.on_stderr,
                .nonblock_flag = LINUX_O_NONBLOCK,
            };
        }

        struct linux_iovec
        {
            uint64_t base;
            uint64_t length;
        };

        template <typename Transfer>
        posix_result transfer_io_vector(const linux_syscall_context& c, const uint64_t iov_addr, const int iovcnt, const Transfer& transfer)
        {
            constexpr int LINUX_UIO_MAXIOV = 1024;
            if (iovcnt < 0 || iovcnt > LINUX_UIO_MAXIOV)
            {
                return posix_errno::einval;
            }

            uint64_t transferred = 0;
            for (int i = 0; i < iovcnt; ++i)
            {
                linux_iovec vector{};
                const auto vector_addr = iov_addr + (static_cast<uint64_t>(i) * sizeof(vector));
                if (!c.emu_ref.memory.is_readable_range(vector_addr, sizeof(vector)) ||
                    !c.emu_ref.memory.try_read_memory(vector_addr, &vector, sizeof(vector)))
                {
                    return posix_partial_transfer(transferred, posix_errno::efault);
                }

                const auto result = transfer(vector.base, vector.length);
                if (const auto* error = std::get_if<posix_errno>(&result))
                {
                    return posix_partial_transfer(transferred, *error);
                }

                const auto count = std::get<uint64_t>(result);
                transferred += count;
                if (count < vector.length)
                {
                    break;
                }
            }

            return transferred;
        }

        posix_fd make_memory_file_fd(std::string path, std::string content, const int flags)
        {
            posix_fd fd{};
            fd.type = fd_type::memory_file;
            fd.guest_path = path;
            fd.host_path = std::move(path);
            fd.memory_file = std::make_shared<posix_memory_fd>();
            fd.memory_file->content = std::move(content);
            fd.flags = flags;
            return fd;
        }

        size_t read_memory_file_at(const posix_fd& fd, uint8_t* buffer, const size_t count, const size_t offset)
        {
            const auto& content = fd.memory_file->content;
            if (count == 0)
            {
                return 0;
            }

            if (offset >= content.size())
            {
                return 0;
            }

            const auto bytes_read = std::min(count, content.size() - offset);
            std::ranges::copy_n(content.begin() + static_cast<std::ptrdiff_t>(offset), static_cast<std::ptrdiff_t>(bytes_read), buffer);
            return bytes_read;
        }

        uint8_t file_type_to_d_type(const std::filesystem::file_type type)
        {
            // Linux DT_* values
            constexpr uint8_t DT_UNKNOWN = 0;
            constexpr uint8_t DT_FIFO = 1;
            constexpr uint8_t DT_CHR = 2;
            constexpr uint8_t DT_DIR = 4;
            constexpr uint8_t DT_BLK = 6;
            constexpr uint8_t DT_REG = 8;
            constexpr uint8_t DT_LNK = 10;
            constexpr uint8_t DT_SOCK = 12;

            switch (type)
            {
            case std::filesystem::file_type::directory:
                return DT_DIR;
            case std::filesystem::file_type::regular:
                return DT_REG;
            case std::filesystem::file_type::symlink:
                return DT_LNK;
            case std::filesystem::file_type::block:
                return DT_BLK;
            case std::filesystem::file_type::character:
                return DT_CHR;
            case std::filesystem::file_type::fifo:
                return DT_FIFO;
            case std::filesystem::file_type::socket:
                return DT_SOCK;
            default:
                return DT_UNKNOWN;
            }
        }

#if defined(_WIN32)
        bool get_windows_file_id(const std::filesystem::path& path, uint64_t& dev, uint64_t& ino)
        {
            HANDLE h = CreateFileW(path.wstring().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);

            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h, &info))
            {
                CloseHandle(h);
                return false;
            }

            dev = static_cast<uint64_t>(info.dwVolumeSerialNumber);
            ino = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | static_cast<uint64_t>(info.nFileIndexLow);

            CloseHandle(h);
            return true;
        }
#endif

        uint64_t get_inode_for_path(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            uint64_t dev = 0;
            uint64_t ino = 0;
            if (get_windows_file_id(path, dev, ino))
            {
                return ino;
            }
#else
            struct stat st{};
            if (lstat(path.string().c_str(), &st) == 0)
            {
                return static_cast<uint64_t>(st.st_ino);
            }
#endif

            return 0;
        }

        std::vector<posix_directory_entry> list_directory(const std::filesystem::path& dir_path)
        {
            std::vector<posix_directory_entry> entries{};

            // Include "." and ".." first to match Linux behavior.
            entries.push_back(
                {.inode = get_inode_for_path(dir_path), .type = file_type_to_d_type(std::filesystem::file_type::directory), .name = "."});

            const auto parent = dir_path.parent_path().empty() ? dir_path : dir_path.parent_path();
            entries.push_back(
                {.inode = get_inode_for_path(parent), .type = file_type_to_d_type(std::filesystem::file_type::directory), .name = ".."});

            std::error_code ec{};
            for (const auto& de : std::filesystem::directory_iterator(dir_path, ec))
            {
                if (ec)
                {
                    break;
                }

                const auto& p = de.path();
                const auto type = de.symlink_status(ec).type();
                if (ec)
                {
                    ec.clear();
                }

                posix_directory_entry entry{};
                entry.inode = get_inode_for_path(p);
                entry.type = file_type_to_d_type(type);
                entry.name = p.filename().string();
                entries.push_back(std::move(entry));
            }

            return entries;
        }

        size_t align_up_8(const size_t value)
        {
            return (value + 7) & ~static_cast<size_t>(7);
        }

        bool open_flags_mutate_file(const int flags)
        {
            const auto access = flags & 3;
            return access == LINUX_O_WRONLY || access == LINUX_O_RDWR || (flags & (LINUX_O_CREAT | LINUX_O_TRUNC)) != 0;
        }

        bool fd_allows_read(const posix_fd& fd)
        {
            return fd.type != fd_type::file || (fd.flags & 3) != LINUX_O_WRONLY;
        }

        std::optional<std::string> resolve_guest_path_name_at(const linux_syscall_context& c, const int dirfd,
                                                              const std::string& guest_path, int64_t& linux_error)
        {
            const auto directory_fd = dirfd == LINUX_AT_FDCWD ? std::nullopt : std::optional{dirfd};

            posix_path_error error{};
            auto resolved = posix_resolve_guest_path_at(c.proc.fds, c.proc.current_working_directory, directory_fd, guest_path, error);
            linux_error = error == posix_path_error::not_a_directory ? LINUX_ENOTDIR : LINUX_EBADF;
            return resolved;
        }

        bool is_read_only_guest_path_at(const linux_syscall_context& c, const int dirfd, const std::string& guest_path)
        {
            int64_t resolve_error{};
            const auto resolved_guest = resolve_guest_path_name_at(c, dirfd, guest_path, resolve_error);
            return resolved_guest.has_value() && c.emu_ref.file_sys.is_read_only_guest_path(*resolved_guest);
        }

        std::optional<std::filesystem::path> resolve_guest_path_at(const linux_syscall_context& c, const int dirfd,
                                                                   const std::string& guest_path, int64_t& linux_error)
        {
            linux_error = LINUX_EBADF;

            if (guest_path.starts_with('/') || guest_path.starts_with('\\'))
            {
                const auto normalized_guest = posix_file_system::normalize_guest_path_string(guest_path);
                if (procfs::is_procfs_path(normalized_guest) || procfs::is_procfs_symlink(normalized_guest))
                {
                    linux_error = LINUX_ENOENT;
                    return std::nullopt;
                }
                return c.emu_ref.file_sys.translate(normalized_guest);
            }

            if (dirfd == LINUX_AT_FDCWD)
            {
                const auto normalized_guest = posix_file_system::resolve_guest_path_string(c.proc.current_working_directory, guest_path);
                if (procfs::is_procfs_path(normalized_guest) || procfs::is_procfs_symlink(normalized_guest))
                {
                    linux_error = LINUX_ENOENT;
                    return std::nullopt;
                }
                return c.emu_ref.file_sys.translate(normalized_guest);
            }

            auto* fd_entry = c.proc.fds.get(dirfd);
            if (!fd_entry)
            {
                return std::nullopt;
            }

            if (fd_entry->type != fd_type::directory)
            {
                linux_error = LINUX_ENOTDIR;
                return std::nullopt;
            }

            if (fd_entry->host_path.empty())
            {
                return std::nullopt;
            }

            if (!fd_entry->guest_path.empty())
            {
                const auto normalized_guest = posix_file_system::resolve_guest_path_string(fd_entry->guest_path, guest_path);
                if (procfs::is_procfs_path(normalized_guest) || procfs::is_procfs_symlink(normalized_guest))
                {
                    linux_error = LINUX_ENOENT;
                    return std::nullopt;
                }
                return c.emu_ref.file_sys.translate(normalized_guest);
            }

            return c.emu_ref.file_sys.translate_relative_to(fd_entry->host_path, guest_path);
        }

        void open_at(const linux_syscall_context& c, const int dirfd, const uint64_t path_addr, const int flags, const uint32_t mode)
        {
            const auto guest_path = read_string<char>(c.emu, path_addr);
            if (guest_path.empty())
            {
                write_linux_syscall_result(c, -LINUX_ENOENT);
                return;
            }

            int64_t resolve_error{};
            auto resolved_guest = resolve_guest_path_name_at(c, dirfd, guest_path, resolve_error);
            if (!resolved_guest.has_value())
            {
                write_linux_syscall_result(c, -resolve_error);
                return;
            }

            // Intercept procfs paths after guest-path resolution so dirfd/cwd-relative
            // opens are handled by the emulator, not the host /proc filesystem.
            if (procfs::is_procfs_path(*resolved_guest))
            {
                if (open_flags_mutate_file(flags))
                {
                    write_linux_syscall_result(c, -LINUX_EACCES);
                    return;
                }

                auto content = procfs::generate_content(c.emu_ref, *resolved_guest);
                if (!content)
                {
                    write_linux_syscall_result(c, -LINUX_ENOENT);
                    return;
                }

                auto new_fd = make_memory_file_fd(*resolved_guest, std::move(*content), flags);

                const auto fd_num = c.proc.fds.allocate(std::move(new_fd), (flags & LINUX_O_CLOEXEC) != 0);
                write_linux_syscall_result(c, fd_num);
                return;
            }

            const auto access = posix_access_mode_from_flags(flags);
            if (!access)
            {
                write_linux_syscall_result(c, -LINUX_EINVAL);
                return;
            }

            const posix_open_request request{
                .guest_path = std::move(*resolved_guest),
                .access = *access,
                .create = (flags & LINUX_O_CREAT) != 0,
                .exclusive = (flags & LINUX_O_EXCL) != 0,
                .truncate = (flags & LINUX_O_TRUNC) != 0,
                .append = (flags & LINUX_O_APPEND) != 0,
                .directory = (flags & LINUX_O_DIRECTORY) != 0,
                .close_on_exec = (flags & LINUX_O_CLOEXEC) != 0,
                .creation_mode = mode,
                .guest_flags = flags,
            };

            write_linux_syscall_result(c, posix_open(c.proc.fds, c.emu_ref.file_sys, request));
        }

#pragma pack(push, 1)

        struct linux_dirent64_header
        {
            uint64_t d_ino;
            int64_t d_off;
            uint16_t d_reclen;
            uint8_t d_type;
        };

#pragma pack(pop)

        void fill_linux_stat_from_host(linux_stat& ls, const struct stat& host_stat)
        {
            memset(&ls, 0, sizeof(ls));

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
            ls.st_atime_nsecs = static_cast<uint64_t>(host_stat.st_atimespec.tv_nsec);
            ls.st_mtime_sec = static_cast<uint64_t>(host_stat.st_mtimespec.tv_sec);
            ls.st_mtime_nsecs = static_cast<uint64_t>(host_stat.st_mtimespec.tv_nsec);
            ls.st_ctime_sec = static_cast<uint64_t>(host_stat.st_ctimespec.tv_sec);
            ls.st_ctime_nsecs = static_cast<uint64_t>(host_stat.st_ctimespec.tv_nsec);
#elif defined(_WIN32)
            ls.st_atime_sec = static_cast<uint64_t>(host_stat.st_atime);
            ls.st_atime_nsecs = 0;
            ls.st_mtime_sec = static_cast<uint64_t>(host_stat.st_mtime);
            ls.st_mtime_nsecs = 0;
            ls.st_ctime_sec = static_cast<uint64_t>(host_stat.st_ctime);
            ls.st_ctime_nsecs = 0;
#else
            ls.st_atime_sec = static_cast<uint64_t>(host_stat.st_atim.tv_sec);
            ls.st_atime_nsecs = static_cast<uint64_t>(host_stat.st_atim.tv_nsec);
            ls.st_mtime_sec = static_cast<uint64_t>(host_stat.st_mtim.tv_sec);
            ls.st_mtime_nsecs = static_cast<uint64_t>(host_stat.st_mtim.tv_nsec);
            ls.st_ctime_sec = static_cast<uint64_t>(host_stat.st_ctim.tv_sec);
            ls.st_ctime_nsecs = static_cast<uint64_t>(host_stat.st_ctim.tv_nsec);
#endif
        }

        bool fill_linux_stat(linux_stat& ls, const std::filesystem::path& host_path, const bool follow_symlinks = true)
        {
            struct stat host_stat{};

            if (follow_symlinks)
            {
                if (stat(host_path.string().c_str(), &host_stat) != 0)
                {
                    memset(&ls, 0, sizeof(ls));
                    return false;
                }
            }
            else
            {
                if (lstat(host_path.string().c_str(), &host_stat) != 0)
                {
                    memset(&ls, 0, sizeof(ls));
                    return false;
                }
            }

            fill_linux_stat_from_host(ls, host_stat);

#if defined(_WIN32)
            uint64_t dev = 0;
            uint64_t ino = 0;

            if (get_windows_file_id(host_path, dev, ino))
            {
                ls.st_dev = dev;
                ls.st_ino = ino;
            }
#endif

            return true;
        }

        bool fill_memory_file_stat(const linux_syscall_context& c, const posix_fd& fd, linux_stat& ls)
        {
            if (!fd.memory_file)
            {
                return false;
            }

            const auto content_size = fd.memory_file->content.size();
            if (!procfs::stat_procfs(c.emu_ref, fd.host_path, ls))
            {
                ls.st_mode = 0100444;
                ls.st_nlink = 1;
                ls.st_uid = c.proc.uid;
                ls.st_gid = c.proc.gid;
                ls.st_ino = static_cast<uint64_t>(std::hash<std::string>{}(fd.host_path));
            }

            ls.st_size = static_cast<int64_t>(content_size);
            ls.st_blksize = 1024;
            ls.st_blocks = static_cast<int64_t>((content_size + 511) / 512);
            return true;
        }
    }

    void sys_read(const linux_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
        const auto count = get_linux_syscall_argument(c.emu, 2);

        const auto* fd_entry = c.proc.fds.get(fd);
        if (fd_entry && fd_entry->type == fd_type::socket && linux_socket_has_host_socket(*fd_entry))
        {
            write_linux_syscall_result(c, linux_socket_recv_guest_buffer(c, fd, buf_addr, static_cast<size_t>(count), 0));
            return;
        }

        write_linux_syscall_result(c, posix_read(io_context(c), fd, buf_addr, count));
    }

    void sys_write(const linux_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
        const auto count = get_linux_syscall_argument(c.emu, 2);

        const auto* fd_entry = c.proc.fds.get(fd);
        if (fd_entry && fd_entry->type == fd_type::socket && linux_socket_has_host_socket(*fd_entry))
        {
            write_linux_syscall_result(c, linux_socket_send_guest_buffer(c, fd, buf_addr, static_cast<size_t>(count), 0));
            return;
        }

        write_linux_syscall_result(c, posix_write(io_context(c), fd, buf_addr, count));
    }

    void sys_open(const linux_syscall_context& c)
    {
        open_at(c, LINUX_AT_FDCWD, get_linux_syscall_argument(c.emu, 0), static_cast<int>(get_linux_syscall_argument(c.emu, 1)),
                static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 2)));
    }

    void sys_close(const linux_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto result = posix_close(c.proc.fds, fd);
        if (std::holds_alternative<uint64_t>(result))
        {
            cleanup_linux_socket_state(fd);
        }

        write_linux_syscall_result(c, result);
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

        if (fd_entry->type == fd_type::memory_file)
        {
            if (!fill_memory_file_stat(c, *fd_entry, ls))
            {
                write_linux_syscall_result(c, -LINUX_EBADF);
                return;
            }
        }
        else if (!fd_entry->host_path.empty())
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
        const auto whence = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 2));
        write_linux_syscall_result(c, posix_lseek(c.proc.fds, fd, offset, whence));
    }

    void sys_access(const linux_syscall_context& c)
    {
        const auto path_addr = get_linux_syscall_argument(c.emu, 0);
        // mode is arg 1, unused for now

        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);

        // Intercept procfs paths after resolving relative names against guest cwd.
        if (resolved_guest.has_value() && (procfs::is_procfs_path(*resolved_guest) || procfs::is_procfs_symlink(*resolved_guest)))
        {
            write_linux_syscall_result(c, 0);
            return;
        }

        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

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
        open_at(c, static_cast<int>(get_linux_syscall_argument(c.emu, 0)), get_linux_syscall_argument(c.emu, 1),
                static_cast<int>(get_linux_syscall_argument(c.emu, 2)), static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 3)));
    }

    void sys_stat(const linux_syscall_context& c)
    {
        const auto path_addr = get_linux_syscall_argument(c.emu, 0);
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);

        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);

        // Intercept procfs paths after guest-path resolution.
        if (resolved_guest.has_value() && procfs::is_procfs_path(*resolved_guest))
        {
            linux_stat ls{};
            if (procfs::stat_procfs(c.emu_ref, *resolved_guest, ls))
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

        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

        linux_stat ls{};
        if (!fill_linux_stat(ls, host_path))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        c.emu.write_memory(buf_addr, &ls, sizeof(ls));
        write_linux_syscall_result(c, 0);
    }

    void sys_lstat(const linux_syscall_context& c)
    {
        const auto path_addr = get_linux_syscall_argument(c.emu, 0);
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);

        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);

        // Intercept procfs paths after guest-path resolution (important for symlinks like /proc/self/exe)
        if (resolved_guest.has_value() && (procfs::is_procfs_path(*resolved_guest) || procfs::is_procfs_symlink(*resolved_guest)))
        {
            linux_stat ls{};
            if (procfs::stat_procfs(c.emu_ref, *resolved_guest, ls))
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

        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

        linux_stat ls{};
        if (!fill_linux_stat(ls, host_path, false))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

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
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type == fd_type::socket && linux_socket_has_host_socket(*fd_entry))
        {
            write_linux_syscall_result(c, linux_socket_recv_guest_buffer(c, fd, buf_addr, count, 0));
            return;
        }

        if (!fd_allows_read(*fd_entry))
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type == fd_type::memory_file)
        {
            if (!fd_entry->memory_file)
            {
                write_linux_syscall_result(c, -LINUX_EBADF);
                return;
            }
            if (offset < 0)
            {
                write_linux_syscall_result(c, -LINUX_EINVAL);
                return;
            }

            std::vector<uint8_t> buffer(count);
            const auto bytes_read = read_memory_file_at(*fd_entry, buffer.data(), count, static_cast<size_t>(offset));
            if (bytes_read > 0)
            {
                c.emu.write_memory(buf_addr, buffer.data(), bytes_read);
            }

            write_linux_syscall_result(c, static_cast<int64_t>(bytes_read));
            return;
        }

        if (!fd_entry->handle)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        const auto saved_pos = ftell(fd_entry->handle);
        fseek(fd_entry->handle, static_cast<long>(offset), SEEK_SET); // NOLINT(google-runtime-int)

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

        const auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type == fd_type::socket && linux_socket_has_host_socket(*fd_entry))
        {
            write_linux_syscall_result(c, linux_socket_writev_from_guest(c, fd, iov_addr, iovcnt));
            return;
        }

        write_linux_syscall_result(c, transfer_io_vector(c, iov_addr, iovcnt, [&c, fd](const uint64_t base, const uint64_t length) {
                                       return posix_write(io_context(c), fd, base, length);
                                   }));
    }

    void sys_dup(const linux_syscall_context& c)
    {
        const auto oldfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        write_linux_syscall_result(c, posix_dup(c.proc.fds, oldfd, 0, false, LINUX_DESCRIPTOR_LIMIT));
    }

    void sys_dup2(const linux_syscall_context& c)
    {
        const auto oldfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto newfd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        write_linux_syscall_result(c, posix_dup2(c.proc.fds, oldfd, newfd, LINUX_DESCRIPTOR_LIMIT));
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

        const auto result = posix_dup2(c.proc.fds, oldfd, newfd, LINUX_DESCRIPTOR_LIMIT);
        if (std::holds_alternative<uint64_t>(result))
        {
            c.proc.fds.set_close_on_exec(newfd, (flags & LINUX_O_CLOEXEC) != 0);
        }

        write_linux_syscall_result(c, result);
    }

    void sys_fcntl(const linux_syscall_context& c)
    {
        constexpr int LINUX_F_DUPFD = 0;
        constexpr int LINUX_F_GETFD = 1;
        constexpr int LINUX_F_SETFD = 2;
        constexpr int LINUX_F_GETFL = 3;
        constexpr int LINUX_F_SETFL = 4;
        constexpr int LINUX_F_DUPFD_CLOEXEC = 1030;

        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto cmd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto arg = get_linux_syscall_argument(c.emu, 2);

        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        switch (cmd)
        {
        case LINUX_F_DUPFD:
        case LINUX_F_DUPFD_CLOEXEC:
            write_linux_syscall_result(
                c, posix_dup(c.proc.fds, fd, static_cast<int64_t>(arg), cmd == LINUX_F_DUPFD_CLOEXEC, LINUX_DESCRIPTOR_LIMIT));
            break;
        case LINUX_F_GETFD:
            write_linux_syscall_result(c, posix_get_descriptor_flags(c.proc.fds, fd));
            break;
        case LINUX_F_SETFD:
            write_linux_syscall_result(c, posix_set_descriptor_flags(c.proc.fds, fd, arg));
            break;
        case LINUX_F_GETFL:
            write_linux_syscall_result(c, posix_get_status_flags(c.proc.fds, fd));
            break;
        case LINUX_F_SETFL:
            fd_entry->flags = static_cast<int>(arg);
            linux_socket_apply_fd_flags(fd, *fd_entry);
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

        const auto cwd = posix_file_system::normalize_guest_path_string(c.proc.current_working_directory);
        const auto cwd_len = cwd.size() + 1;

        if (size < cwd_len)
        {
            write_linux_syscall_result(c, -LINUX_ERANGE);
            return;
        }

        c.emu.write_memory(buf_addr, cwd.c_str(), cwd_len);
        write_linux_syscall_result(c, static_cast<int64_t>(cwd_len));
    }

    void sys_chdir(const linux_syscall_context& c)
    {
        const auto path_addr = get_linux_syscall_argument(c.emu, 0);
        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved_guest.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        if (procfs::is_procfs_path(*resolved_guest))
        {
            linux_stat ls{};
            if (!procfs::stat_procfs(c.emu_ref, *resolved_guest, ls))
            {
                write_linux_syscall_result(c, -LINUX_ENOENT);
                return;
            }
            if ((ls.st_mode & 0170000) != 040000)
            {
                write_linux_syscall_result(c, -LINUX_ENOTDIR);
                return;
            }

            c.proc.current_working_directory = std::move(*resolved_guest);
            write_linux_syscall_result(c, 0);
            return;
        }

        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        if (!std::filesystem::exists(*resolved))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }
        if (!std::filesystem::is_directory(*resolved))
        {
            write_linux_syscall_result(c, -LINUX_ENOTDIR);
            return;
        }

        c.proc.current_working_directory = std::move(*resolved_guest);
        write_linux_syscall_result(c, 0);
    }

    void sys_fchdir(const linux_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }
        if (fd_entry->type != fd_type::directory)
        {
            write_linux_syscall_result(c, -LINUX_ENOTDIR);
            return;
        }
        if (fd_entry->guest_path.empty())
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        c.proc.current_working_directory = posix_file_system::normalize_guest_path_string(fd_entry->guest_path);
        write_linux_syscall_result(c, 0);
    }

    void sys_readlink(const linux_syscall_context& c)
    {
        const auto path_addr = get_linux_syscall_argument(c.emu, 0);
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
        const auto bufsiz = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));

        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);

        // Intercept procfs symlinks after resolving relative names against guest cwd.
        if (resolved_guest.has_value() && procfs::is_procfs_symlink(*resolved_guest))
        {
            auto target = procfs::resolve_symlink(c.emu_ref, *resolved_guest);
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

        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        std::error_code ec{};
        const auto target = std::filesystem::read_symlink(*resolved, ec);
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
        const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto path_addr = get_linux_syscall_argument(c.emu, 1);
        const auto buf_addr = get_linux_syscall_argument(c.emu, 2);
        const auto bufsiz = static_cast<size_t>(get_linux_syscall_argument(c.emu, 3));

        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, dirfd, guest_path, resolve_error);

        // Intercept procfs symlinks after guest-path resolution.
        if (resolved_guest.has_value() && procfs::is_procfs_symlink(*resolved_guest))
        {
            auto target = procfs::resolve_symlink(c.emu_ref, *resolved_guest);
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

        auto resolved = resolve_guest_path_at(c, dirfd, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        std::error_code ec{};
        const auto target = std::filesystem::read_symlink(*resolved, ec);
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
        const auto dirp_addr = get_linux_syscall_argument(c.emu, 1);
        const auto count = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));

        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type != fd_type::directory)
        {
            write_linux_syscall_result(c, -LINUX_ENOTDIR);
            return;
        }

        if (!fd_entry->directory_listing)
        {
            fd_entry->directory_listing = list_directory(fd_entry->host_path);
        }

        const auto& entries = *fd_entry->directory_listing;
        auto& offset = fd_entry->directory_position;

        size_t written = 0;

        while (offset < entries.size())
        {
            const auto& entry = entries.at(static_cast<size_t>(offset));
            const auto name_bytes = entry.name.size() + 1; // include trailing NUL
            const auto reclen = align_up_8(sizeof(linux_dirent64_header) + name_bytes);

            if (written + reclen > count)
            {
                if (written == 0)
                {
                    write_linux_syscall_result(c, -LINUX_EINVAL);
                    return;
                }
                break;
            }

            linux_dirent64_header hdr{};
            hdr.d_ino = entry.inode;
            hdr.d_off = static_cast<int64_t>(offset) + 1;
            hdr.d_reclen = static_cast<uint16_t>(reclen);
            hdr.d_type = entry.type;

            std::vector<uint8_t> record(reclen, 0);
            memcpy(record.data(), &hdr, sizeof(hdr));
            std::ranges::copy(entry.name, reinterpret_cast<char*>(record.data() + sizeof(hdr)));

            c.emu.write_memory(dirp_addr + written, record.data(), record.size());

            written += reclen;
            ++offset;
        }

        write_linux_syscall_result(c, static_cast<int64_t>(written));
    }

    void sys_fsync(const linux_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->handle)
        {
            fflush(fd_entry->handle);
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_fdatasync(const linux_syscall_context& c)
    {
        // Our emulation does not currently distinguish metadata sync from data sync.
        sys_fsync(c);
    }

    void sys_newfstatat(const linux_syscall_context& c)
    {
        const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto path_addr = get_linux_syscall_argument(c.emu, 1);
        const auto buf_addr = get_linux_syscall_argument(c.emu, 2);
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 3));

        const auto guest_path = read_string<char>(c.emu, path_addr);

        constexpr int LINUX_AT_EMPTY_PATH = 0x1000;
        constexpr int LINUX_AT_SYMLINK_NOFOLLOW = 0x100;
        if (guest_path.empty() && (flags & LINUX_AT_EMPTY_PATH))
        {
            linux_stat ls{};
            if (dirfd == LINUX_AT_FDCWD)
            {
                fill_linux_stat(ls, c.emu_ref.file_sys.translate("/"));
            }
            else
            {
                auto* fd_entry = c.proc.fds.get(dirfd);
                if (!fd_entry)
                {
                    write_linux_syscall_result(c, -LINUX_EBADF);
                    return;
                }

                if (fd_entry->type == fd_type::memory_file)
                {
                    if (!fill_memory_file_stat(c, *fd_entry, ls))
                    {
                        write_linux_syscall_result(c, -LINUX_EBADF);
                        return;
                    }
                }
                else if (!fd_entry->host_path.empty())
                {
                    fill_linux_stat(ls, fd_entry->host_path);
                }
                else
                {
                    ls.st_mode = 0020666;
                    ls.st_rdev = 0x0501;
                }
            }

            c.emu.write_memory(buf_addr, &ls, sizeof(ls));
            write_linux_syscall_result(c, 0);
            return;
        }

        if (guest_path.empty())
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, dirfd, guest_path, resolve_error);

        // Intercept procfs paths after guest-path resolution.
        if (resolved_guest.has_value() && procfs::is_procfs_path(*resolved_guest))
        {
            linux_stat ls{};
            if (procfs::stat_procfs(c.emu_ref, *resolved_guest, ls))
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

        auto resolved = resolve_guest_path_at(c, dirfd, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto& host_path = *resolved;

        if (!std::filesystem::exists(host_path))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        linux_stat ls{};
        const bool follow_symlinks = (flags & LINUX_AT_SYMLINK_NOFOLLOW) == 0;

        if (!fill_linux_stat(ls, host_path, follow_symlinks))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        c.emu.write_memory(buf_addr, &ls, sizeof(ls));
        write_linux_syscall_result(c, 0);
    }

    void sys_rename(const linux_syscall_context& c)
    {
        const auto oldpath_addr = get_linux_syscall_argument(c.emu, 0);
        const auto newpath_addr = get_linux_syscall_argument(c.emu, 1);

        const auto old_guest = read_string<char>(c.emu, oldpath_addr);
        const auto new_guest = read_string<char>(c.emu, newpath_addr);

        if (is_read_only_guest_path_at(c, LINUX_AT_FDCWD, old_guest) || is_read_only_guest_path_at(c, LINUX_AT_FDCWD, new_guest))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

        int64_t old_resolve_error{};
        auto old_resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, old_guest, old_resolve_error);
        if (!old_resolved.has_value())
        {
            write_linux_syscall_result(c, -old_resolve_error);
            return;
        }

        int64_t new_resolve_error{};
        auto new_resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, new_guest, new_resolve_error);
        if (!new_resolved.has_value())
        {
            write_linux_syscall_result(c, -new_resolve_error);
            return;
        }

        const auto& old_host = *old_resolved;
        const auto& new_host = *new_resolved;

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
        const auto olddirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto oldpath_addr = get_linux_syscall_argument(c.emu, 1);
        const auto newdirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
        const auto newpath_addr = get_linux_syscall_argument(c.emu, 3);
        (void)get_linux_syscall_argument(c.emu, 4);

        const auto old_guest = read_string<char>(c.emu, oldpath_addr);
        const auto new_guest = read_string<char>(c.emu, newpath_addr);

        if (is_read_only_guest_path_at(c, olddirfd, old_guest) || is_read_only_guest_path_at(c, newdirfd, new_guest))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

        int64_t old_resolve_error{};
        auto old_resolved = resolve_guest_path_at(c, olddirfd, old_guest, old_resolve_error);
        if (!old_resolved.has_value())
        {
            write_linux_syscall_result(c, -old_resolve_error);
            return;
        }

        int64_t new_resolve_error{};
        auto new_resolved = resolve_guest_path_at(c, newdirfd, new_guest, new_resolve_error);
        if (!new_resolved.has_value())
        {
            write_linux_syscall_result(c, -new_resolve_error);
            return;
        }

        const auto& old_host = *old_resolved;
        const auto& new_host = *new_resolved;

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
        if (is_read_only_guest_path_at(c, LINUX_AT_FDCWD, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

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
        const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto path_addr = get_linux_syscall_argument(c.emu, 1);
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, dirfd, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto& host_path = *resolved;
        if (is_read_only_guest_path_at(c, dirfd, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

        constexpr int LINUX_AT_REMOVEDIR = 0x200;

        std::error_code ec{};
        if (flags & LINUX_AT_REMOVEDIR)
        {
            if (!std::filesystem::exists(host_path))
            {
                write_linux_syscall_result(c, -LINUX_ENOENT);
                return;
            }

            if (!std::filesystem::is_directory(host_path))
            {
                write_linux_syscall_result(c, -LINUX_ENOTDIR);
                return;
            }

            if (!std::filesystem::remove(host_path, ec))
            {
                write_linux_syscall_result(c, -LINUX_ENOTEMPTY);
                return;
            }
        }
        else if (!std::filesystem::remove(host_path, ec))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_mkdir(const linux_syscall_context& c)
    {
        const auto path_addr = get_linux_syscall_argument(c.emu, 0);
        // mode is arg 1

        const auto guest_path = read_string<char>(c.emu, path_addr);
        if (is_read_only_guest_path_at(c, LINUX_AT_FDCWD, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

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
        const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto path_addr = get_linux_syscall_argument(c.emu, 1);

        const auto guest_path = read_string<char>(c.emu, path_addr);
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, dirfd, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto& host_path = *resolved;
        if (is_read_only_guest_path_at(c, dirfd, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

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
        if (is_read_only_guest_path_at(c, LINUX_AT_FDCWD, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

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
        int64_t resolve_error{};
        auto resolved_link_guest = resolve_guest_path_name_at(c, LINUX_AT_FDCWD, linkpath_guest, resolve_error);
        if (!resolved_link_guest.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        if (c.emu_ref.file_sys.is_read_only_guest_path(*resolved_link_guest))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }
        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, linkpath_guest, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& linkpath_host = *resolved;
        const auto host_target = c.emu_ref.file_sys.translate_symlink_target(target, *resolved_link_guest);

        std::error_code ec{};
        std::filesystem::create_symlink(host_target, linkpath_host, ec);
        if (ec)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_symlinkat(const linux_syscall_context& c)
    {
        const auto target_addr = get_linux_syscall_argument(c.emu, 0);
        const auto newdirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto linkpath_addr = get_linux_syscall_argument(c.emu, 2);

        const auto target = read_string<char>(c.emu, target_addr);
        const auto linkpath_guest = read_string<char>(c.emu, linkpath_addr);
        int64_t resolve_error{};
        auto resolved_link_guest = resolve_guest_path_name_at(c, newdirfd, linkpath_guest, resolve_error);
        if (!resolved_link_guest.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        auto resolved = resolve_guest_path_at(c, newdirfd, linkpath_guest, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto& linkpath_host = *resolved;
        if (c.emu_ref.file_sys.is_read_only_guest_path(*resolved_link_guest))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }
        const auto host_target = c.emu_ref.file_sys.translate_symlink_target(target, *resolved_link_guest);

        std::error_code ec{};
        std::filesystem::create_symlink(host_target, linkpath_host, ec);
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
        if (is_read_only_guest_path_at(c, LINUX_AT_FDCWD, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

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

        if (fd_entry->read_only_mapping)
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
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
        const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto path_addr = get_linux_syscall_argument(c.emu, 1);
        const auto mode = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 2));
        (void)get_linux_syscall_argument(c.emu, 3);

        const auto guest_path = read_string<char>(c.emu, path_addr);
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, dirfd, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto& host_path = *resolved;
        if (is_read_only_guest_path_at(c, dirfd, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

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
        if (is_read_only_guest_path_at(c, LINUX_AT_FDCWD, guest_path))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

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

        if (fd_entry->read_only_mapping)
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
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
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type == fd_type::socket && linux_socket_has_host_socket(*fd_entry))
        {
            write_linux_syscall_result(c, linux_socket_send_guest_buffer(c, fd, buf_addr, count, 0));
            return;
        }

        if (!fd_entry->handle)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->read_only_mapping)
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

        const auto saved_pos = ftell(fd_entry->handle);
        fseek(fd_entry->handle, static_cast<long>(offset), SEEK_SET); // NOLINT(google-runtime-int)

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

        const auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type == fd_type::socket && linux_socket_has_host_socket(*fd_entry))
        {
            write_linux_syscall_result(c, linux_socket_readv_to_guest(c, fd, iov_addr, iovcnt));
            return;
        }

        write_linux_syscall_result(c, transfer_io_vector(c, iov_addr, iovcnt, [&c, fd](const uint64_t base, const uint64_t length) {
                                       return posix_read(io_context(c), fd, base, length);
                                   }));
    }

    void sys_faccessat(const linux_syscall_context& c)
    {
        // faccessat(int dirfd, const char *pathname, int mode, int flags)
        const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto path_addr = get_linux_syscall_argument(c.emu, 1);
        // mode is arg 2, flags is arg 3

        const auto guest_path = read_string<char>(c.emu, path_addr);

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, dirfd, guest_path, resolve_error);

        // Intercept procfs paths after guest-path resolution.
        if (resolved_guest.has_value() && (procfs::is_procfs_path(*resolved_guest) || procfs::is_procfs_symlink(*resolved_guest)))
        {
            write_linux_syscall_result(c, 0);
            return;
        }

        auto resolved = resolve_guest_path_at(c, dirfd, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto& host_path = *resolved;

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
        int64_t resolve_error{};
        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }
        const auto& host_path = *resolved;

        if (!std::filesystem::exists(host_path))
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        // Linux struct statfs (120 bytes on x86-64)
        // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
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
        // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

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

        // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
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
        // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

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

} // namespace sogen
