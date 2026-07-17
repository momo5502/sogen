#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"
#include "../linux_stat.hpp"
#include "../procfs.hpp"

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
    bool linux_socket_has_host_socket(const linux_fd& fd_entry);
    void linux_socket_apply_fd_flags(int fd, const linux_fd& fd_entry);
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
        constexpr int LINUX_O_TRUNC = 01000;
        constexpr int LINUX_O_APPEND = 02000;
        constexpr int LINUX_O_DIRECTORY = 0200000;
        constexpr int LINUX_O_CLOEXEC = 02000000;

        int64_t host_read_fd(const int fd, void* buffer, const size_t count)
        {
#if defined(_WIN32)
            constexpr auto max_count = static_cast<size_t>(std::numeric_limits<unsigned int>::max());
            const auto request = static_cast<unsigned int>(std::min(count, max_count));
            return static_cast<int64_t>(_read(fd, buffer, request));
#else
            return static_cast<int64_t>(::read(fd, buffer, count));
#endif
        }

#if defined(_WIN32)
        bool host_pipe_would_block(const int fd)
        {
            auto* const os_handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
            if (os_handle == INVALID_HANDLE_VALUE)
            {
                errno = EBADF;
                return false;
            }

            DWORD available = 0;
            if (PeekNamedPipe(os_handle, nullptr, 0, nullptr, &available, nullptr) != 0)
            {
                if (available == 0)
                {
                    errno = EAGAIN;
                    return true;
                }

                return false;
            }

            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE)
            {
                return false;
            }

            errno = EIO;
            return false;
        }
#endif

        int64_t host_write_fd(const int fd, const void* buffer, const size_t count)
        {
#if defined(_WIN32)
            constexpr auto max_count = static_cast<size_t>(std::numeric_limits<unsigned int>::max());
            const auto request = static_cast<unsigned int>(std::min(count, max_count));
            return static_cast<int64_t>(_write(fd, buffer, request));
#else
            return static_cast<int64_t>(::write(fd, buffer, count));
#endif
        }

        linux_fd make_memory_file_fd(std::string path, std::string content, const int flags)
        {
            linux_fd fd{};
            fd.type = fd_type::memory_file;
            fd.guest_path = path;
            fd.host_path = std::move(path);
            fd.memory_file = std::make_shared<linux_memory_fd>();
            fd.memory_file->content = std::move(content);
            fd.flags = flags;
            fd.close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;
            return fd;
        }

        size_t read_memory_file_at(const linux_fd& fd, uint8_t* buffer, const size_t count, const size_t offset)
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

        size_t read_memory_file(linux_fd& fd, uint8_t* buffer, const size_t count)
        {
            const auto bytes_read = read_memory_file_at(fd, buffer, count, fd.memory_file->offset);
            fd.memory_file->offset += bytes_read;
            return bytes_read;
        }

        std::optional<size_t> seek_memory_file(linux_fd& fd, const int64_t offset, const int whence)
        {
            const auto content_size = static_cast<int64_t>(fd.memory_file->content.size());
            auto base = int64_t{0};
            if (whence == 1)
            {
                base = static_cast<int64_t>(fd.memory_file->offset);
            }
            else if (whence == 2)
            {
                base = content_size;
            }

            if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
                (offset < 0 && base < std::numeric_limits<int64_t>::min() - offset))
            {
                return std::nullopt;
            }

            const auto target = base + offset;
            if (target < 0)
            {
                return std::nullopt;
            }

            fd.memory_file->offset = static_cast<size_t>(target);
            return fd.memory_file->offset;
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

        std::vector<linux_cached_dir_entry> build_cached_dir_entries(const std::filesystem::path& dir_path)
        {
            std::vector<linux_cached_dir_entry> entries{};

            // Include "." and ".." first to match Linux behavior.
            entries.push_back(
                {.ino = get_inode_for_path(dir_path), .d_type = file_type_to_d_type(std::filesystem::file_type::directory), .name = "."});

            const auto parent = dir_path.parent_path().empty() ? dir_path : dir_path.parent_path();
            entries.push_back(
                {.ino = get_inode_for_path(parent), .d_type = file_type_to_d_type(std::filesystem::file_type::directory), .name = ".."});

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

                linux_cached_dir_entry entry{};
                entry.ino = get_inode_for_path(p);
                entry.d_type = file_type_to_d_type(type);
                entry.name = p.filename().string();
                entries.push_back(std::move(entry));
            }

            return entries;
        }

        void init_directory_fd_state(linux_process_context& proc, const int fd, const std::filesystem::path& dir_path)
        {
            proc.directory_entries[fd] = build_cached_dir_entries(dir_path);
            proc.directory_offsets[fd] = 0;
        }

        void cleanup_closed_fd(linux_process_context& proc, const int fd)
        {
            proc.directory_entries.erase(fd);
            proc.directory_offsets.erase(fd);
            proc.epoll_instances.erase(fd);

            std::vector<linux_epoll_instance*> cleaned_instances{};
            for (auto& [epfd, instance] : proc.epoll_instances)
            {
                (void)epfd;
                if (!instance || std::ranges::find(cleaned_instances, instance.get()) != cleaned_instances.end())
                {
                    continue;
                }

                cleaned_instances.push_back(instance.get());
                auto& entries = instance->entries;
                entries.erase(std::ranges::remove_if(entries, [fd](const linux_epoll_entry& entry) { return entry.fd == fd; }).begin(),
                              entries.end());
            }
        }

        std::shared_ptr<linux_epoll_instance> get_epoll_instance(linux_process_context& proc, const int fd)
        {
            const auto it = proc.epoll_instances.find(fd);
            return it != proc.epoll_instances.end() ? it->second : std::shared_ptr<linux_epoll_instance>{};
        }

        int dup_fd_preserve_epoll_state(linux_process_context& proc, const int oldfd, const int minimum_fd = 0)
        {
            const auto* old_entry = proc.fds.get(oldfd);
            const bool duplicates_epoll = old_entry != nullptr && old_entry->type == fd_type::epoll;
            const auto epoll_instance = duplicates_epoll ? get_epoll_instance(proc, oldfd) : std::shared_ptr<linux_epoll_instance>{};
            if (duplicates_epoll && !epoll_instance)
            {
                return -1;
            }
            const auto result = proc.fds.dup_fd(oldfd, minimum_fd);
            if (result >= 0 && duplicates_epoll)
            {
                proc.epoll_instances[result] = epoll_instance;
            }

            return result;
        }

        int dup2_fd_cleanup_overwrite(linux_process_context& proc, const int oldfd, const int newfd)
        {
            const auto* old_entry = proc.fds.get(oldfd);
            const bool duplicates_epoll = oldfd != newfd && old_entry != nullptr && old_entry->type == fd_type::epoll;
            const auto epoll_instance = duplicates_epoll ? get_epoll_instance(proc, oldfd) : std::shared_ptr<linux_epoll_instance>{};
            if (duplicates_epoll && !epoll_instance)
            {
                return -1;
            }
            const bool replaces_existing_fd = oldfd != newfd && proc.fds.get(newfd) != nullptr;
            const auto result = proc.fds.dup2_fd(oldfd, newfd);
            if (result < 0)
            {
                return result;
            }

            if (replaces_existing_fd)
            {
                cleanup_closed_fd(proc, newfd);
            }

            if (duplicates_epoll)
            {
                proc.epoll_instances[newfd] = epoll_instance;
            }

            return result;
        }

        size_t align_up_8(const size_t value)
        {
            return (value + 7) & ~static_cast<size_t>(7);
        }

        int64_t map_host_errno_to_linux(const int host_errno)
        {
            switch (host_errno)
            {
            case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
                return LINUX_EAGAIN;
            case EINTR:
                return LINUX_EINTR;
            case EBADF:
                return LINUX_EBADF;
            case EINVAL:
                return LINUX_EINVAL;
            case EPIPE:
                return LINUX_EPIPE;
            case ENOENT:
                return LINUX_ENOENT;
            case EACCES:
                return LINUX_EACCES;
            case ENOSPC:
                return LINUX_ENOSPC;
            case EISDIR:
                return LINUX_EISDIR;
            case ENOTDIR:
                return LINUX_ENOTDIR;
            default:
                return LINUX_EIO;
            }
        }

        bool open_flags_mutate_file(const int flags)
        {
            const auto access = flags & 3;
            return access == LINUX_O_WRONLY || access == LINUX_O_RDWR || (flags & (LINUX_O_CREAT | LINUX_O_TRUNC)) != 0;
        }

        bool fd_allows_read(const linux_fd& fd)
        {
            return fd.type != fd_type::file || (fd.flags & 3) != LINUX_O_WRONLY;
        }

        std::optional<std::string> resolve_guest_path_name_at(const linux_syscall_context& c, const int dirfd,
                                                              const std::string& guest_path, int64_t& linux_error)
        {
            linux_error = LINUX_EBADF;

            if (guest_path.starts_with('/') || guest_path.starts_with('\\'))
            {
                return linux_file_system::normalize_guest_path_string(guest_path);
            }

            if (dirfd == LINUX_AT_FDCWD)
            {
                return linux_file_system::resolve_guest_path_string(c.proc.current_working_directory, guest_path);
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

            if (fd_entry->guest_path.empty())
            {
                return std::nullopt;
            }

            return linux_file_system::resolve_guest_path_string(fd_entry->guest_path, guest_path);
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
                const auto normalized_guest = linux_file_system::normalize_guest_path_string(guest_path);
                if (procfs::is_procfs_path(normalized_guest) || procfs::is_procfs_symlink(normalized_guest))
                {
                    linux_error = LINUX_ENOENT;
                    return std::nullopt;
                }
                return c.emu_ref.file_sys.translate(normalized_guest);
            }

            if (dirfd == LINUX_AT_FDCWD)
            {
                const auto normalized_guest = linux_file_system::resolve_guest_path_string(c.proc.current_working_directory, guest_path);
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
                const auto normalized_guest = linux_file_system::resolve_guest_path_string(fd_entry->guest_path, guest_path);
                if (procfs::is_procfs_path(normalized_guest) || procfs::is_procfs_symlink(normalized_guest))
                {
                    linux_error = LINUX_ENOENT;
                    return std::nullopt;
                }
                return c.emu_ref.file_sys.translate(normalized_guest);
            }

            return c.emu_ref.file_sys.translate_relative_to(fd_entry->host_path, guest_path);
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

        const char* translate_open_mode(const int flags, const bool file_existed)
        {
            const auto access = flags & 3;

            if (flags & LINUX_O_APPEND)
            {
                return (access == LINUX_O_RDWR) ? "a+b" : "ab";
            }

            if ((flags & LINUX_O_TRUNC) || ((flags & LINUX_O_CREAT) && !file_existed))
            {
                return (access == LINUX_O_RDWR) ? "w+b" : "wb";
            }

            switch (access)
            {
            case LINUX_O_WRONLY:
                return "wb";
            case LINUX_O_RDWR:
                return "r+b";
            default:
                return "rb";
            }
        }

        FILE* open_host_file(const std::filesystem::path& host_path, const int flags)
        {
            const auto path_str = host_path.string();
            const auto access = flags & 3;
            const auto file_existed = std::filesystem::exists(host_path);

#if defined(_WIN32)
            int host_flags = _O_BINARY;
            DWORD desired_access = 0;
            switch (access)
            {
            case LINUX_O_WRONLY:
                desired_access = GENERIC_WRITE;
                host_flags |= _O_WRONLY;
                break;
            case LINUX_O_RDWR:
                desired_access = GENERIC_READ | GENERIC_WRITE;
                host_flags |= _O_RDWR;
                break;
            default:
                desired_access = GENERIC_READ;
                host_flags |= _O_RDONLY;
                break;
            }

            if (flags & LINUX_O_APPEND)
            {
                host_flags |= _O_APPEND;
            }

            DWORD creation_disposition = OPEN_EXISTING;
            if ((flags & LINUX_O_CREAT) && (flags & LINUX_O_TRUNC))
            {
                creation_disposition = CREATE_ALWAYS;
            }
            else if (flags & LINUX_O_CREAT)
            {
                creation_disposition = OPEN_ALWAYS;
            }
            else if (flags & LINUX_O_TRUNC)
            {
                creation_disposition = TRUNCATE_EXISTING;
            }

            auto* const host_handle = CreateFileA(path_str.c_str(), desired_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                  nullptr, creation_disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (host_handle == INVALID_HANDLE_VALUE)
            {
                return nullptr;
            }

            const auto host_fd = _open_osfhandle(reinterpret_cast<intptr_t>(host_handle), host_flags);
            if (host_fd < 0)
            {
                CloseHandle(host_handle);
                return nullptr;
            }

            auto* handle = _fdopen(host_fd, translate_open_mode(flags, file_existed));
#else
            int host_flags = 0;
            switch (access)
            {
            case LINUX_O_WRONLY:
                host_flags |= O_WRONLY;
                break;
            case LINUX_O_RDWR:
                host_flags |= O_RDWR;
                break;
            default:
                host_flags |= O_RDONLY;
                break;
            }

            if (flags & LINUX_O_CREAT)
            {
                host_flags |= O_CREAT;
            }
            if (flags & LINUX_O_TRUNC)
            {
                host_flags |= O_TRUNC;
            }
            if (flags & LINUX_O_APPEND)
            {
                host_flags |= O_APPEND;
            }

            const auto host_fd = open(path_str.c_str(), host_flags, 0666);
            if (host_fd < 0)
            {
                return nullptr;
            }

            auto* handle = fdopen(host_fd, translate_open_mode(flags, file_existed));
#endif
            if (!handle)
            {
#if defined(_WIN32)
                _close(host_fd);
#else
                close(host_fd);
#endif
                return nullptr;
            }

            setvbuf(handle, nullptr, _IONBF, 0);
            return handle;
        }

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

        bool fill_memory_file_stat(const linux_syscall_context& c, const linux_fd& fd, linux_stat& ls)
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
        const auto count = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));

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

        if (fd_entry->host_path == "/dev/stdin")
        {
            // stdin: return 0 (EOF) for now
            write_linux_syscall_result(c, 0);
            return;
        }

        if (fd_entry->type == fd_type::memory_file)
        {
            if (!fd_entry->memory_file)
            {
                write_linux_syscall_result(c, -LINUX_EBADF);
                return;
            }

            std::vector<uint8_t> buffer(count);
            const auto bytes_read = read_memory_file(*fd_entry, buffer.data(), count);
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

        if (fd_entry->type == fd_type::directory)
        {
            write_linux_syscall_result(c, -LINUX_EISDIR);
            return;
        }

        if (fd_entry->type == fd_type::pipe_write)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        std::vector<uint8_t> buffer(count);
        int64_t bytes_read = 0;

        if (fd_entry->type == fd_type::pipe_read)
        {
            const auto host_fd = fileno(fd_entry->handle);
#if defined(_WIN32)
            constexpr int LINUX_O_NONBLOCK = 04000;
            if ((fd_entry->flags & LINUX_O_NONBLOCK) != 0 && host_pipe_would_block(host_fd))
            {
                write_linux_syscall_result(c, -LINUX_EAGAIN);
                return;
            }
#endif
            errno = 0;
            bytes_read = host_read_fd(host_fd, buffer.data(), count);
            if (bytes_read < 0)
            {
                write_linux_syscall_result(c, -map_host_errno_to_linux(errno));
                return;
            }
        }
        else
        {
            const auto stdio_bytes = fread(buffer.data(), 1, count, fd_entry->handle);
            bytes_read = static_cast<int64_t>(stdio_bytes);
        }

        if (bytes_read > 0)
        {
            c.emu.write_memory(buf_addr, buffer.data(), static_cast<size_t>(bytes_read));
        }

        write_linux_syscall_result(c, bytes_read);
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

        if (fd_entry->type == fd_type::socket && linux_socket_has_host_socket(*fd_entry))
        {
            write_linux_syscall_result(c, linux_socket_send_guest_buffer(c, fd, buf_addr, count, 0));
            return;
        }

        std::vector<uint8_t> buffer(count);
        c.emu.read_memory(buf_addr, buffer.data(), count);

        if (fd == 1 || fd == 2)
        {
            // stdout / stderr: invoke callback if set, otherwise write to host
            const auto sv = std::string_view(reinterpret_cast<const char*>(buffer.data()), count);
            auto& callback = (fd == 1) ? c.emu_ref.on_stdout : c.emu_ref.on_stderr;

            if (callback)
            {
                callback(sv);
            }
            else
            {
                auto* target = (fd == 1) ? stdout : stderr;
                fwrite(buffer.data(), 1, count, target);
                fflush(target);
            }

            write_linux_syscall_result(c, static_cast<int64_t>(count));
            return;
        }

        if (!fd_entry->handle)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type == fd_type::directory || fd_entry->type == fd_type::pipe_read)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (fd_entry->type == fd_type::pipe_write)
        {
            const auto host_fd = fileno(fd_entry->handle);
            errno = 0;
            const auto written = host_write_fd(host_fd, buffer.data(), count);
            if (written < 0)
            {
                write_linux_syscall_result(c, -map_host_errno_to_linux(errno));
                return;
            }

            write_linux_syscall_result(c, written);
            return;
        }

        if (fd_entry->read_only_mapping)
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
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

        int64_t resolve_error{};
        auto resolved_guest = resolve_guest_path_name_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved_guest.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        // Intercept procfs paths after guest-path resolution so cwd-relative
        // opens from /proc/self never escape to the host /proc filesystem.
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

            const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
            write_linux_syscall_result(c, fd_num);
            return;
        }

        auto resolved = resolve_guest_path_at(c, LINUX_AT_FDCWD, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto read_only_mapping = c.emu_ref.file_sys.is_read_only_guest_path(*resolved_guest);
        if (read_only_mapping && open_flags_mutate_file(flags))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

        const auto& host_path = *resolved;
        if (flags & LINUX_O_DIRECTORY)
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

            linux_fd new_fd{};
            new_fd.type = fd_type::directory;
            new_fd.host_path = host_path.string();
            new_fd.guest_path = resolved_guest.value_or(std::string{});
            new_fd.flags = flags;
            new_fd.read_only_mapping = read_only_mapping;
            new_fd.close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;

            const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
            init_directory_fd_state(c.proc, fd_num, host_path);
            write_linux_syscall_result(c, fd_num);
            return;
        }

        auto* handle = open_host_file(host_path, flags);
        if (!handle)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        linux_fd new_fd{};
        new_fd.type = fd_type::file;
        new_fd.host_path = host_path.string();
        new_fd.guest_path = resolved_guest.value_or(std::string{});
        new_fd.handle = handle;
        new_fd.flags = flags;
        new_fd.read_only_mapping = read_only_mapping;
        new_fd.close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;

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

        cleanup_closed_fd(c.proc, fd);
        cleanup_linux_socket_state(fd);

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
        const auto whence = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
        auto* fd_entry = c.proc.fds.get(fd);

        if (!fd_entry)
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

            const auto pos = seek_memory_file(*fd_entry, offset, whence);
            write_linux_syscall_result(c, pos.has_value() ? static_cast<int64_t>(*pos) : -LINUX_EINVAL);
            return;
        }

        if (!fd_entry->handle)
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

        if (fseek(fd_entry->handle, static_cast<long>(offset), origin) /* NOLINT(google-runtime-int) */ != 0)
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
        const auto dirfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto path_addr = get_linux_syscall_argument(c.emu, 1);
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
        // mode is arg 3

        const auto guest_path = read_string<char>(c.emu, path_addr);

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

            const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
            write_linux_syscall_result(c, fd_num);
            return;
        }

        auto resolved = resolve_guest_path_at(c, dirfd, guest_path, resolve_error);
        if (!resolved.has_value())
        {
            write_linux_syscall_result(c, -resolve_error);
            return;
        }

        const auto& host_path = *resolved;
        const auto read_only_mapping = c.emu_ref.file_sys.is_read_only_guest_path(*resolved_guest);
        if (read_only_mapping && open_flags_mutate_file(flags))
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
            return;
        }

        if (flags & LINUX_O_DIRECTORY)
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

            linux_fd new_fd{};
            new_fd.type = fd_type::directory;
            new_fd.host_path = host_path.string();
            new_fd.guest_path = resolved_guest.value_or(std::string{});
            new_fd.flags = flags;
            new_fd.read_only_mapping = read_only_mapping;
            new_fd.close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;

            const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
            init_directory_fd_state(c.proc, fd_num, host_path);
            write_linux_syscall_result(c, fd_num);
            return;
        }

        auto* handle = open_host_file(host_path, flags);
        if (!handle)
        {
            write_linux_syscall_result(c, -LINUX_ENOENT);
            return;
        }

        linux_fd new_fd{};
        new_fd.type = fd_type::file;
        new_fd.host_path = host_path.string();
        new_fd.guest_path = resolved_guest.value_or(std::string{});
        new_fd.handle = handle;
        new_fd.flags = flags;
        new_fd.read_only_mapping = read_only_mapping;
        new_fd.close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;

        const auto fd_num = c.proc.fds.allocate(std::move(new_fd));
        write_linux_syscall_result(c, fd_num);
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

        auto* fd_entry = c.proc.fds.get(fd);
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

        if (fd_entry->read_only_mapping)
        {
            write_linux_syscall_result(c, -LINUX_EACCES);
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

            std::vector<uint8_t> buffer(static_cast<size_t>(iov_len));
            c.emu.read_memory(iov_base, buffer.data(), static_cast<size_t>(iov_len));

            if (fd == 1 || fd == 2)
            {
                const auto sv = std::string_view(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(iov_len));
                auto& callback = (fd == 1) ? c.emu_ref.on_stdout : c.emu_ref.on_stderr;

                if (callback)
                {
                    callback(sv);
                }
                else
                {
                    auto* target = (fd == 1) ? stdout : stderr;
                    fwrite(buffer.data(), 1, static_cast<size_t>(iov_len), target);
                    fflush(target);
                }

                total_written += static_cast<int64_t>(iov_len);
            }
            else if (fd_entry->handle)
            {
                const auto written = fwrite(buffer.data(), 1, static_cast<size_t>(iov_len), fd_entry->handle);
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
        const auto result = dup_fd_preserve_epoll_state(c.proc, oldfd);
        write_linux_syscall_result(c, result >= 0 ? result : -LINUX_EBADF);
    }

    void sys_dup2(const linux_syscall_context& c)
    {
        const auto oldfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto newfd = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto result = dup2_fd_cleanup_overwrite(c.proc, oldfd, newfd);
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

        const auto result = dup2_fd_cleanup_overwrite(c.proc, oldfd, newfd);
        if (result < 0)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

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

        constexpr int LINUX_F_DUPFD = 0;
        constexpr int LINUX_F_GETFD = 1;
        constexpr int LINUX_F_SETFD = 2;
        constexpr int LINUX_F_GETFL = 3;
        constexpr int LINUX_F_SETFL = 4;
        constexpr int LINUX_F_DUPFD_CLOEXEC = 1030;

        if ((cmd == LINUX_F_DUPFD || cmd == LINUX_F_DUPFD_CLOEXEC) && arg > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        switch (cmd)
        {
        case LINUX_F_DUPFD: {
            const auto minimum_fd = static_cast<int>(arg);
            if (minimum_fd < 0)
            {
                write_linux_syscall_result(c, -LINUX_EINVAL);
                break;
            }

            const auto new_fd = dup_fd_preserve_epoll_state(c.proc, fd, minimum_fd);
            write_linux_syscall_result(c, new_fd >= 0 ? new_fd : -LINUX_EMFILE);
            break;
        }
        case LINUX_F_DUPFD_CLOEXEC: {
            const auto minimum_fd = static_cast<int>(arg);
            if (minimum_fd < 0)
            {
                write_linux_syscall_result(c, -LINUX_EINVAL);
                break;
            }

            const auto new_fd = dup_fd_preserve_epoll_state(c.proc, fd, minimum_fd);
            if (new_fd >= 0)
            {
                c.proc.fds.set_close_on_exec(new_fd, true);
            }
            write_linux_syscall_result(c, new_fd >= 0 ? new_fd : -LINUX_EMFILE);
            break;
        }
        case LINUX_F_GETFD:
            write_linux_syscall_result(c, fd_entry->close_on_exec ? 1 : 0);
            break;
        case LINUX_F_SETFD:
            fd_entry->close_on_exec = (arg & 1) != 0;
            write_linux_syscall_result(c, 0);
            break;
        case LINUX_F_GETFL:
            write_linux_syscall_result(c, fd_entry->flags);
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

        const auto cwd = linux_file_system::normalize_guest_path_string(c.proc.current_working_directory);
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

        c.proc.current_working_directory = linux_file_system::normalize_guest_path_string(fd_entry->guest_path);
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

        if (!c.proc.directory_entries.contains(fd))
        {
            init_directory_fd_state(c.proc, fd, fd_entry->host_path);
        }

        auto entries_it = c.proc.directory_entries.find(fd);
        if (entries_it == c.proc.directory_entries.end())
        {
            write_linux_syscall_result(c, 0);
            return;
        }

        auto& entries = entries_it->second;
        auto& offset = c.proc.directory_offsets[fd];

        size_t written = 0;

        while (offset < entries.size())
        {
            const auto& entry = entries.at(offset);
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
            hdr.d_ino = entry.ino;
            hdr.d_off = static_cast<int64_t>(offset + 1);
            hdr.d_reclen = static_cast<uint16_t>(reclen);
            hdr.d_type = entry.d_type;

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

        auto* fd_entry = c.proc.fds.get(fd);
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

        if (!fd_allows_read(*fd_entry))
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

            if (fd_entry->host_path == "/dev/stdin")
            {
                // stdin: EOF
                break;
            }

            if (fd_entry->type == fd_type::memory_file)
            {
                if (!fd_entry->memory_file)
                {
                    write_linux_syscall_result(c, -LINUX_EBADF);
                    return;
                }

                std::vector<uint8_t> buffer(static_cast<size_t>(iov_len));
                const auto bytes_read = read_memory_file(*fd_entry, buffer.data(), static_cast<size_t>(iov_len));
                if (bytes_read > 0)
                {
                    c.emu.write_memory(iov_base, buffer.data(), bytes_read);
                    total_read += static_cast<int64_t>(bytes_read);
                }

                if (bytes_read < iov_len)
                {
                    break;
                }
                continue;
            }

            if (!fd_entry->handle)
            {
                write_linux_syscall_result(c, -LINUX_EBADF);
                return;
            }

            std::vector<uint8_t> buffer(static_cast<size_t>(iov_len));
            const auto bytes_read = fread(buffer.data(), 1, static_cast<size_t>(iov_len), fd_entry->handle);

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
