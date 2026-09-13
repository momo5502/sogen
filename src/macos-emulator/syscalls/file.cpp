#include "../std_include.hpp"
#include "../macos_emulator.hpp"
#include "../macos_stat.hpp"
#include "../macos_syscall_utils.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_dirent_type;
    using namespace macos_errno;
    using namespace macos_guard;
    using namespace macos_open;
    using namespace macos_stat_mode;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        int read_syscall_fd(const macos_syscall_context& c, const size_t index)
        {
            return static_cast<int>(static_cast<int32_t>(get_macos_syscall_argument(c, index)));
        }

        int64_t map_filesystem_error(const std::error_code& error)
        {
            const auto condition = error.default_error_condition();
            if (condition.category() == std::generic_category())
            {
                return map_host_errno_to_macos(condition.value());
            }

            return MACOS_EIO;
        }

        int64_t read_guest_path(const macos_syscall_context& c, const uint64_t address, std::string& path)
        {
            path.clear();

            for (uint64_t i = 0; i < MACOS_PATH_MAX; ++i)
            {
                char character{};
                if (!c.emu_ref.memory.try_read_memory(address + i, &character, sizeof(character)))
                {
                    return MACOS_EFAULT;
                }

                if (character == '\0')
                {
                    return path.empty() ? MACOS_ENOENT : 0;
                }

                path.push_back(character);
            }

            return MACOS_ENAMETOOLONG;
        }

        std::filesystem::path resolve_guest_path(const macos_syscall_context& c, const std::string& guest_path)
        {
            return c.emu_ref.file_sys.translate_guest_relative_to(c.proc.current_working_directory, guest_path);
        }

        bool is_read_only_guest_path(const macos_syscall_context& c, const std::string& guest_path)
        {
            return c.emu_ref.file_sys.is_read_only_guest_path(
                guest_file_system::resolve_guest_path_string(c.proc.current_working_directory, guest_path));
        }

        uint8_t macos_type_byte(const std::filesystem::file_type type)
        {
            switch (type)
            {
            case std::filesystem::file_type::regular:
                return MACOS_DT_REG;
            case std::filesystem::file_type::directory:
                return MACOS_DT_DIR;
            case std::filesystem::file_type::symlink:
                return MACOS_DT_LNK;
            case std::filesystem::file_type::character:
                return MACOS_DT_CHR;
            case std::filesystem::file_type::block:
                return MACOS_DT_BLK;
            case std::filesystem::file_type::fifo:
                return MACOS_DT_FIFO;
            case std::filesystem::file_type::socket:
                return MACOS_DT_SOCK;
            default:
                return MACOS_DT_UNKNOWN;
            }
        }

        uint16_t macos_permission_bits(const std::filesystem::perms permissions)
        {
            return static_cast<uint16_t>(static_cast<uint32_t>(permissions & std::filesystem::perms::mask) & MACOS_S_IPERM);
        }

        macos_stat64 make_synthetic_stat(const macos_syscall_context& c, const uint16_t mode, const int64_t size)
        {
            macos_stat64 result{};
            result.st_mode = mode;
            result.st_nlink = 1;
            result.st_uid = c.proc.uid;
            result.st_gid = c.proc.gid;
            result.st_size = size;
            result.st_blocks = size / 512 + ((size % 512) != 0 ? 1 : 0);
            result.st_blksize = static_cast<int32_t>(MACOS_PAGE_SIZE);
            return result;
        }

        int64_t stat_host_path(const macos_syscall_context& c, const std::filesystem::path& host_path, const bool follow_symlinks,
                               macos_stat64& result)
        {
            std::error_code error{};
            const auto link_status = std::filesystem::symlink_status(host_path, error);
            if (link_status.type() == std::filesystem::file_type::not_found)
            {
                return error ? map_filesystem_error(error) : MACOS_ENOENT;
            }

            if (error)
            {
                return map_filesystem_error(error);
            }

            if (!follow_symlinks && link_status.type() == std::filesystem::file_type::symlink)
            {
                const auto target = std::filesystem::read_symlink(host_path, error);
                if (error)
                {
                    return map_filesystem_error(error);
                }

                result = make_synthetic_stat(c, static_cast<uint16_t>(MACOS_S_IFLNK | macos_permission_bits(link_status.permissions())),
                                             static_cast<int64_t>(target.string().size()));
                return 0;
            }

            struct compat_stat host{};
            if (!compat_stat(host_path, &host))
            {
                return MACOS_ENOENT;
            }

            result = host_stat_to_macos(host, c.proc.uid, c.proc.gid);
            return 0;
        }

        // dyld cross-checks the st_ino from stat64 against the object id it gets from getattrlist and
        // against the dyld_file= / executable_file= pairs in apple[]. Reporting the host inode in one
        // place and a synthetic id in the others makes it conclude the image moved underneath it, so
        // every path that fills a macos_stat64 for a named file goes through here.
        void apply_guest_identity(const macos_syscall_context& c, const std::string_view guest_path, macos_stat64& result)
        {
            if (guest_path.empty())
            {
                return;
            }

            const auto identity = c.emu_ref.identities.acquire(std::string{guest_path});
            result.st_dev = static_cast<int32_t>(identity.fsid_dev);
            result.st_ino = identity.object_id;
        }

        void write_stat_result(const macos_syscall_context& c, const uint64_t buffer, const macos_stat64& result)
        {
            if (!c.emu_ref.memory.try_write_memory(buffer, &result, sizeof(result)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            write_macos_syscall_result(c, 0);
        }

        void stat_path_syscall(const macos_syscall_context& c, const size_t path_index, const size_t buffer_index,
                               const bool follow_symlinks)
        {
            std::string guest_path{};
            const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, path_index), guest_path);
            if (path_error != 0)
            {
                write_macos_syscall_error(c, path_error);
                return;
            }

            macos_stat64 result{};
            const auto error = stat_host_path(c, resolve_guest_path(c, guest_path), follow_symlinks, result);
            if (error != 0)
            {
                write_macos_syscall_error(c, error);
                return;
            }

            apply_guest_identity(c, guest_path, result);
            write_stat_result(c, get_macos_syscall_argument(c, buffer_index), result);
        }

        // Only ever reaches fdopen, never fopen. fdopen cannot truncate - the descriptor is already
        // open - so the mode string only has to agree with the descriptor's access mode. Handing "wb"
        // to fopen instead would empty a file the guest asked to open with a bare O_WRONLY, which
        // POSIX preserves.
        const char* host_stream_mode(const int32_t flags)
        {
            const auto access = flags & MACOS_O_ACCMODE;

            if ((flags & MACOS_O_APPEND) != 0)
            {
                return access == MACOS_O_RDWR ? "a+b" : "ab";
            }

            switch (access)
            {
            case MACOS_O_WRONLY:
                return "wb";
            case MACOS_O_RDWR:
                return "r+b";
            default:
                return "rb";
            }
        }

        int open_host_descriptor(const std::string& host_path, const int32_t flags)
        {
            const auto access = flags & MACOS_O_ACCMODE;

            // Truncation and creation are requested explicitly, never implied by a mode string: that is
            // the whole reason the descriptor is opened here rather than through fopen.
#if defined(_WIN32)
            auto host_flags = _O_BINARY;
            if (access == MACOS_O_WRONLY)
            {
                host_flags |= _O_WRONLY;
            }
            else if (access == MACOS_O_RDWR)
            {
                host_flags |= _O_RDWR;
            }
            else
            {
                host_flags |= _O_RDONLY;
            }

            if ((flags & MACOS_O_CREAT) != 0)
            {
                host_flags |= _O_CREAT;
            }

            if ((flags & MACOS_O_TRUNC) != 0)
            {
                host_flags |= _O_TRUNC;
            }

            if ((flags & MACOS_O_APPEND) != 0)
            {
                host_flags |= _O_APPEND;
            }

            return _open(host_path.c_str(), host_flags, _S_IREAD | _S_IWRITE);
#else
            auto host_flags = O_RDONLY;
            if (access == MACOS_O_WRONLY)
            {
                host_flags = O_WRONLY;
            }
            else if (access == MACOS_O_RDWR)
            {
                host_flags = O_RDWR;
            }

            if ((flags & MACOS_O_CREAT) != 0)
            {
                host_flags |= O_CREAT;
            }

            if ((flags & MACOS_O_TRUNC) != 0)
            {
                host_flags |= O_TRUNC;
            }

            if ((flags & MACOS_O_APPEND) != 0)
            {
                host_flags |= O_APPEND;
            }

            return ::open(host_path.c_str(), host_flags, 0666);
#endif
        }

        FILE* open_host_file(const std::string& host_path, const int32_t flags)
        {
            const auto host_fd = open_host_descriptor(host_path, flags);
            if (host_fd < 0)
            {
                return nullptr;
            }

#if defined(_WIN32)
            auto* handle = _fdopen(host_fd, host_stream_mode(flags));
#else
            auto* handle = ::fdopen(host_fd, host_stream_mode(flags));
#endif
            if (handle == nullptr)
            {
                const auto failure = errno;
#if defined(_WIN32)
                _close(host_fd);
#else
                ::close(host_fd);
#endif
                errno = failure;
                return nullptr;
            }

            setvbuf(handle, nullptr, _IONBF, 0);
            return handle;
        }

        int allocate_descriptor(const macos_syscall_context& c, guest_fd entry)
        {
            auto guest_path = entry.guest_path;

            const auto fd = c.proc.fds.allocate(std::move(entry));
            if (fd >= MACOS_MAX_OPEN_DESCRIPTORS)
            {
                c.proc.fds.close(fd);
                write_macos_syscall_error(c, MACOS_EMFILE);
                return -1;
            }

            // The guest path rather than the host one. A trace has to show what the guest asked for, and
            // the host path is an emulator detail that would leak the operator's filesystem layout into
            // a trace meant to be shared.
            c.emu_ref.callbacks.on_generic_access("Opening file", guest_path);
            write_macos_syscall_result(c, fd);
            return fd;
        }

        // Resolving a relative path against a directory descriptor. The descriptor's own guest path is
        // what makes this possible without a per-fd cwd: dyld opens /System/Library/dyld and then asks
        // for "dyld_shared_cache_arm64e" relative to it, which is the only way it ever names the cache.
        int64_t resolve_at_directory(const macos_syscall_context& c, const int directory_fd, std::string& guest_path)
        {
            if (guest_path.empty())
            {
                return MACOS_ENOENT;
            }

            if (directory_fd == MACOS_AT_FDCWD || guest_path.front() == '/')
            {
                return 0;
            }

            const auto* entry = c.proc.fds.get(directory_fd);
            if (entry == nullptr)
            {
                return MACOS_EBADF;
            }

            if (entry->type != fd_type::directory)
            {
                return MACOS_ENOTDIR;
            }

            auto base = entry->guest_path;
            if (base.empty())
            {
                return MACOS_EBADF;
            }

            if (base.back() != '/')
            {
                base.push_back('/');
            }

            guest_path = guest_file_system::normalize_guest_path_string(base + guest_path);
            return 0;
        }

        // Returns the descriptor it produced, or -1 having already written the error the caller reports.
        int open_guest_path(const macos_syscall_context& c, const std::string& requested_path, const int32_t flags)
        {
            const auto host_path = resolve_guest_path(c, requested_path);

            // The descriptor remembers where it points, not what the caller typed. F_GETPATH answers a
            // vnode's absolute path on Darwin, and getcwd(3) is built out of open(".") plus F_GETPATH:
            // storing a relative path makes getcwd() answer "." and sends CFURL into an unbounded
            // recursion resolving a relative path against a relative current directory.
            const auto guest_path = guest_file_system::resolve_guest_path_string(c.proc.current_working_directory, requested_path);

            std::error_code error{};
            const auto status = std::filesystem::status(host_path, error);
            const auto exists = !error && status.type() != std::filesystem::file_type::not_found;

            if (!exists && (flags & MACOS_O_CREAT) == 0)
            {
                write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
                return -1;
            }

            const auto read_only = is_read_only_guest_path(c, guest_path);
            const auto writing = (flags & MACOS_O_ACCMODE) != MACOS_O_RDONLY;

            if (exists && status.type() == std::filesystem::file_type::directory)
            {
                if (writing)
                {
                    write_macos_syscall_error(c, MACOS_EISDIR);
                    return -1;
                }

                guest_fd entry{};
                entry.type = fd_type::directory;
                entry.host_path = host_path.string();
                entry.guest_path = guest_path;
                entry.flags = flags;
                entry.close_on_exec = (flags & MACOS_O_CLOEXEC) != 0;
                entry.read_only_mapping = read_only;

                return allocate_descriptor(c, std::move(entry));
            }

            if ((flags & MACOS_O_DIRECTORY) != 0)
            {
                write_macos_syscall_error(c, MACOS_ENOTDIR);
                return -1;
            }

            if (guest_path == MACOS_NULL_DEVICE_PATH && (flags & MACOS_O_DIRECTORY) == 0)
            {
                guest_fd entry{};
                entry.type = fd_type::memory_file;
                entry.memory_file = std::make_shared<guest_memory_fd>();
                entry.host_path = host_path.string();
                entry.guest_path = guest_path;
                entry.flags = flags;
                entry.close_on_exec = (flags & MACOS_O_CLOEXEC) != 0;

                return allocate_descriptor(c, std::move(entry));
            }

            // A character device, fifo or socket reachable through the emulation root would park the
            // emulator inside a blocking host read with nothing left to resume it, and nothing here
            // emulates one. Report the honest "device not configured" rather than opening it.
            if (exists && status.type() != std::filesystem::file_type::regular)
            {
                write_macos_syscall_error(c, MACOS_ENXIO);
                return -1;
            }

            if (exists && (flags & (MACOS_O_CREAT | MACOS_O_EXCL)) == (MACOS_O_CREAT | MACOS_O_EXCL))
            {
                write_macos_syscall_error(c, MACOS_EEXIST);
                return -1;
            }

            if (read_only && writing)
            {
                write_macos_syscall_error(c, MACOS_EACCES);
                return -1;
            }

            errno = 0;
            auto* handle = open_host_file(host_path.string(), flags);
            if (handle == nullptr)
            {
                write_macos_syscall_error(c, errno != 0 ? map_host_errno_to_macos(errno) : MACOS_ENOENT);
                return -1;
            }

            guest_fd entry{};
            entry.type = fd_type::file;
            entry.host_path = host_path.string();
            entry.guest_path = guest_path;
            entry.handle = handle;
            entry.flags = flags;
            entry.close_on_exec = (flags & MACOS_O_CLOEXEC) != 0;
            entry.read_only_mapping = read_only;

            return allocate_descriptor(c, std::move(entry));
        }

        uint64_t host_inode(const std::filesystem::path& path)
        {
            struct compat_stat host{};
            return compat_stat(path, &host) ? host.st_ino : 0;
        }

        std::vector<macos_directory_entry> read_host_directory(const std::filesystem::path& host_path, const std::filesystem::path& root)
        {
            std::vector<macos_directory_entry> entries{};

            entries.push_back({.inode = host_inode(host_path), .type = MACOS_DT_DIR, .name = "."});

            // The emulation root is the guest's "/", whose ".." is itself. Walking to the host parent
            // would stat a directory outside everything translate can produce. Today translate returns
            // the root with a trailing slash, so parent_path() already lands back on the root and this
            // is unreachable; it is kept because that is a lexical accident, and losing the trailing
            // slash would silently start reporting the host directory above the root.
            const auto normalized = host_path.lexically_normal();
            const auto at_root = !root.empty() && normalized == root.lexically_normal();
            const auto parent = (at_root || normalized.parent_path().empty()) ? normalized : normalized.parent_path();
            entries.push_back({.inode = host_inode(parent), .type = MACOS_DT_DIR, .name = ".."});

            std::error_code error{};
            std::filesystem::directory_iterator iterator{host_path, error};
            const std::filesystem::directory_iterator end{};

            // The range-for form uses the throwing increment; a handler that leaves an exception in
            // unicorn's C frames unwinds the whole run, so the traversal is spelled out with the
            // error_code overload instead.
            while (!error && iterator != end)
            {
                std::error_code status_error{};
                const auto type = iterator->symlink_status(status_error).type();

                entries.push_back({.inode = host_inode(iterator->path()),
                                   .type = macos_type_byte(status_error ? std::filesystem::file_type::unknown : type),
                                   .name = iterator->path().filename().string()});

                iterator.increment(error);
            }

            return entries;
        }

        std::string guest_symlink_target(const macos_syscall_context& c, const std::filesystem::path& target)
        {
            const auto& root = c.emu_ref.file_sys.root();
            if (root.empty() || !target.is_absolute())
            {
                return target.generic_string();
            }

            const auto relative = target.lexically_normal().lexically_relative(root.lexically_normal());
            if (relative.empty() || *relative.begin() == "..")
            {
                return target.generic_string();
            }

            return "/" + relative.generic_string();
        }

        void write_statfs_result(const macos_syscall_context& c, const uint64_t buffer)
        {
            macos_statfs64 result{};
            result.f_bsize = static_cast<uint32_t>(MACOS_PAGE_SIZE);
            result.f_iosize = static_cast<int32_t>(MACOS_PAGE_SIZE);

            constexpr std::string_view type_name = "apfs";
            memcpy(result.f_fstypename.data(), type_name.data(), type_name.size());
            result.f_mntonname[0] = '/';

            if (!c.emu_ref.memory.try_write_memory(buffer, &result, sizeof(result)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            write_macos_syscall_result(c, 0);
        }
    }

    macos_stat64 host_stat_to_macos(const struct compat_stat& host, const uint32_t uid, const uint32_t gid)
    {
        const auto to_macos_time = [](const timespec& value) {
            return macos_timespec{.tv_sec = static_cast<int64_t>(value.tv_sec), .tv_nsec = static_cast<int64_t>(value.tv_nsec)};
        };

        macos_stat64 result{};
        result.st_dev = static_cast<int32_t>(host.st_dev);
        result.st_mode = static_cast<uint16_t>(host.st_mode);
        result.st_nlink = static_cast<uint16_t>(host.st_nlink);
        result.st_ino = host.st_ino;
        result.st_uid = uid;
        result.st_gid = gid;
        result.st_rdev = static_cast<int32_t>(host.st_rdev);
        result.st_atimespec = to_macos_time(host.st_atimespec);
        result.st_mtimespec = to_macos_time(host.st_mtimespec);
        result.st_ctimespec = to_macos_time(host.st_ctimespec);

        // No portable host stat reports a creation time, and dyld only compares it against a recorded
        // value, so reusing the inode change time keeps the two consistent within one run.
        result.st_birthtimespec = result.st_ctimespec;

        result.st_size = host.st_size;
        result.st_blocks = host.st_size / 512 + ((host.st_size % 512) != 0 ? 1 : 0);
        result.st_blksize = static_cast<int32_t>(MACOS_PAGE_SIZE);

        return result;
    }

    // POSIX shared memory has no namespace here, and inventing one would mean inventing the system
    // state behind it: the first object libSystem asks for is com.apple.featureflags.shm, whose
    // contents decide which features the process believes are enabled. A guest with no featureflagsd
    // genuinely does not have it, and libSystem falls back to its defaults on ENOENT -- which is the
    // honest answer rather than a convenient one.
    //
    // A sample that uses shm_open to share memory with another process will also get ENOENT. That is a
    // real limitation, not a decision that it should fail; it is logged so a run that depends on it
    // says so.
    void sys_shm_open(const macos_syscall_context& c)
    {
        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        c.emu_ref.log.print(color::gray, "shm_open(\"%s\") has no shared memory namespace to open\n", guest_path.c_str());
        write_macos_syscall_error(c, MACOS_ENOENT);
    }

    void sys_open(const macos_syscall_context& c)
    {
        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        open_guest_path(c, guest_path, static_cast<int32_t>(get_macos_syscall_argument(c, 1)));
    }

    void sys_openat(const macos_syscall_context& c)
    {
        const auto directory_fd = read_syscall_fd(c, 0);

        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 1), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        if (const auto error = resolve_at_directory(c, directory_fd, guest_path); error != 0)
        {
            write_macos_syscall_error(c, error);
            return;
        }

        open_guest_path(c, guest_path, static_cast<int32_t>(get_macos_syscall_argument(c, 2)));
    }

    namespace
    {
        // The guard argument is a pointer to the id the caller wants pinned to the new descriptor. Three
        // things are rejected before the path is even read, all measured against this host's kernel:
        // guardflags that leave out GUARD_REQUIRED, guardflags naming a bit the kernel does not define,
        // and -- the one that is easy to miss -- an open without O_CLOEXEC, because a guarded descriptor
        // is not allowed to survive an exec into a process that never agreed to the guard.
        void open_guarded_guest_path(const macos_syscall_context& c, const uint64_t path_address, const uint64_t guard_address,
                                     const uint32_t guardflags, const int32_t flags)
        {
            if ((guardflags & MACOS_GUARD_REQUIRED) != MACOS_GUARD_REQUIRED || (guardflags & ~MACOS_GUARD_ALL) != 0 ||
                (flags & MACOS_O_CLOEXEC) == 0)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            uint64_t id = 0;
            if (!c.emu_ref.memory.try_read_memory(guard_address, &id, sizeof(id)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            std::string guest_path{};
            const auto path_error = read_guest_path(c, path_address, guest_path);
            if (path_error != 0)
            {
                write_macos_syscall_error(c, path_error);
                return;
            }

            const auto fd = open_guest_path(c, guest_path, flags);
            if (fd < 0)
            {
                return;
            }

            c.proc.fd_guards[fd] = {.id = id, .flags = guardflags};
        }
    }

    void sys_guarded_open_np(const macos_syscall_context& c)
    {
        open_guarded_guest_path(c, get_macos_syscall_argument(c, 0), get_macos_syscall_argument(c, 1),
                                static_cast<uint32_t>(get_macos_syscall_argument(c, 2)),
                                static_cast<int32_t>(get_macos_syscall_argument(c, 3)));
    }

    // dpclass and dpflags pick an iOS content protection class for the new file. A Mac volume has no
    // per-file data protection to choose between, so xnu itself drops both on macOS; libsqlite3 calls
    // this variant unconditionally and reads back what the plain open would have produced.
    void sys_guarded_open_dprotected_np(const macos_syscall_context& c)
    {
        open_guarded_guest_path(c, get_macos_syscall_argument(c, 0), get_macos_syscall_argument(c, 1),
                                static_cast<uint32_t>(get_macos_syscall_argument(c, 2)),
                                static_cast<int32_t>(get_macos_syscall_argument(c, 3)));
    }

    namespace
    {
        void access_guest_path(const macos_syscall_context& c, const std::string& guest_path, const int32_t mode, const bool follow)
        {
            std::error_code error{};
            const auto host_path = resolve_guest_path(c, guest_path);
            const auto status = follow ? std::filesystem::status(host_path, error) : std::filesystem::symlink_status(host_path, error);
            if (error || status.type() == std::filesystem::file_type::not_found)
            {
                write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
                return;
            }

            const auto denies = [&](const int32_t requested, const std::filesystem::perms owner) {
                return (mode & requested) != 0 && (status.permissions() & owner) == std::filesystem::perms::none;
            };

            if (denies(MACOS_R_OK, std::filesystem::perms::owner_read) || denies(MACOS_W_OK, std::filesystem::perms::owner_write) ||
                denies(MACOS_X_OK, std::filesystem::perms::owner_exec))
            {
                write_macos_syscall_error(c, MACOS_EACCES);
                return;
            }

            if ((mode & MACOS_W_OK) != 0 && is_read_only_guest_path(c, guest_path))
            {
                write_macos_syscall_error(c, MACOS_EACCES);
                return;
            }

            write_macos_syscall_result(c, 0);
        }
    }

    void sys_access(const macos_syscall_context& c)
    {
        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        access_guest_path(c, guest_path, static_cast<int32_t>(get_macos_syscall_argument(c, 1)), true);
    }

    // faccessat(fd, path, mode, flag). AT_EACCESS asks for the check to use the effective ids rather
    // than the real ones; sogen runs every guest as one identity, so the two are the same check.
    void sys_faccessat(const macos_syscall_context& c)
    {
        const auto directory_fd = read_syscall_fd(c, 0);

        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 1), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        if (const auto error = resolve_at_directory(c, directory_fd, guest_path); error != 0)
        {
            write_macos_syscall_error(c, error);
            return;
        }

        const auto flags = static_cast<int32_t>(get_macos_syscall_argument(c, 3));
        access_guest_path(c, guest_path, static_cast<int32_t>(get_macos_syscall_argument(c, 2)), (flags & MACOS_AT_SYMLINK_NOFOLLOW) == 0);
    }

    void sys_stat64(const macos_syscall_context& c)
    {
        stat_path_syscall(c, 0, 1, true);
    }

    void sys_lstat64(const macos_syscall_context& c)
    {
        stat_path_syscall(c, 0, 1, false);
    }

    void sys_fstatat64(const macos_syscall_context& c)
    {
        const auto directory_fd = read_syscall_fd(c, 0);
        const auto flags = static_cast<int32_t>(get_macos_syscall_argument(c, 3));

        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 1), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        if (const auto error = resolve_at_directory(c, directory_fd, guest_path); error != 0)
        {
            write_macos_syscall_error(c, error);
            return;
        }

        macos_stat64 result{};
        const auto stat_error = stat_host_path(c, resolve_guest_path(c, guest_path), (flags & MACOS_AT_SYMLINK_NOFOLLOW) == 0, result);
        if (stat_error != 0)
        {
            write_macos_syscall_error(c, stat_error);
            return;
        }

        apply_guest_identity(c, guest_path, result);
        write_stat_result(c, get_macos_syscall_argument(c, 2), result);
    }

    void sys_fstat64(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto buffer = get_macos_syscall_argument(c, 1);

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (entry->type == fd_type::memory_file)
        {
            if (!entry->memory_file)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            write_stat_result(c, buffer,
                              make_synthetic_stat(c, static_cast<uint16_t>(MACOS_S_IFREG | 0644),
                                                  static_cast<int64_t>(entry->memory_file->content.size())));
            return;
        }

        if (entry->host_path.empty() || guest_fd_detail::is_stdio_path(entry->host_path))
        {
            write_stat_result(c, buffer, make_synthetic_stat(c, static_cast<uint16_t>(MACOS_S_IFCHR | 0620), 0));
            return;
        }

        macos_stat64 result{};
        const auto error = stat_host_path(c, std::filesystem::path{entry->host_path}, true, result);
        if (error != 0)
        {
            write_macos_syscall_error(c, error);
            return;
        }

        apply_guest_identity(c, entry->guest_path, result);
        write_stat_result(c, buffer, result);
    }

    void sys_readlink(const macos_syscall_context& c)
    {
        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        const auto buffer = get_macos_syscall_argument(c, 1);
        const auto size = get_macos_syscall_argument(c, 2);

        const auto host_path = resolve_guest_path(c, guest_path);

        std::error_code error{};
        const auto link_status = std::filesystem::symlink_status(host_path, error);
        if (link_status.type() == std::filesystem::file_type::not_found)
        {
            write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
            return;
        }

        if (error)
        {
            write_macos_syscall_error(c, map_filesystem_error(error));
            return;
        }

        if (link_status.type() != std::filesystem::file_type::symlink)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto target = std::filesystem::read_symlink(host_path, error);
        if (error)
        {
            write_macos_syscall_error(c, map_filesystem_error(error));
            return;
        }

        const auto guest_target = guest_symlink_target(c, target);
        const auto count = static_cast<size_t>(std::min<uint64_t>(size, guest_target.size()));

        if (count > 0 && !c.emu_ref.memory.try_write_memory(buffer, guest_target.data(), count))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, static_cast<int64_t>(count));
    }

    namespace
    {
        void make_guest_directory(const macos_syscall_context& c, const std::string& guest_path)
        {
            if (is_read_only_guest_path(c, guest_path))
            {
                write_macos_syscall_error(c, MACOS_EACCES);
                return;
            }

            const auto host_path = resolve_guest_path(c, guest_path);

            std::error_code error{};
            if (std::filesystem::symlink_status(host_path, error).type() != std::filesystem::file_type::not_found)
            {
                write_macos_syscall_error(c, MACOS_EEXIST);
                return;
            }

            // The mode is dropped: an emulation root is the host's own directory tree, and stamping a
            // guest's umask-filtered mode onto it would make a directory the emulator itself can no
            // longer traverse once the guest asks for 0000.
            if (!std::filesystem::create_directory(host_path, error))
            {
                write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_EEXIST);
                return;
            }

            write_macos_syscall_result(c, 0);
        }
    }

    void sys_mkdir(const macos_syscall_context& c)
    {
        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        make_guest_directory(c, guest_path);
    }

    void sys_mkdirat(const macos_syscall_context& c)
    {
        const auto directory_fd = read_syscall_fd(c, 0);

        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 1), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        if (const auto error = resolve_at_directory(c, directory_fd, guest_path); error != 0)
        {
            write_macos_syscall_error(c, error);
            return;
        }

        make_guest_directory(c, guest_path);
    }

    // libsqlite3 renames a journal into place and removes the directory it made for a proxy conch, so
    // the pair below is reached on the same CoreData store path the guarded opens are.
    void sys_rename(const macos_syscall_context& c)
    {
        std::string from_path{};
        if (const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), from_path); path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        std::string to_path{};
        if (const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 1), to_path); path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        if (is_read_only_guest_path(c, from_path) || is_read_only_guest_path(c, to_path))
        {
            write_macos_syscall_error(c, MACOS_EACCES);
            return;
        }

        const auto from_host = resolve_guest_path(c, from_path);
        const auto to_host = resolve_guest_path(c, to_path);

        std::error_code error{};
        if (std::filesystem::symlink_status(from_host, error).type() == std::filesystem::file_type::not_found)
        {
            write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
            return;
        }

        std::filesystem::rename(from_host, to_host, error);
        if (error)
        {
            write_macos_syscall_error(c, map_filesystem_error(error));
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_rmdir(const macos_syscall_context& c)
    {
        std::string guest_path{};
        if (const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path); path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        const auto host_path = resolve_guest_path(c, guest_path);

        std::error_code error{};
        const auto status = std::filesystem::symlink_status(host_path, error);
        if (status.type() == std::filesystem::file_type::not_found)
        {
            write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
            return;
        }

        if (status.type() != std::filesystem::file_type::directory)
        {
            write_macos_syscall_error(c, MACOS_ENOTDIR);
            return;
        }

        if (is_read_only_guest_path(c, guest_path))
        {
            write_macos_syscall_error(c, MACOS_EACCES);
            return;
        }

        // remove() deletes a directory only when it is empty and reports ENOTEMPTY otherwise, which is
        // rmdir's contract; remove_all() would obliterate a tree the guest only asked to unlink.
        if (!std::filesystem::remove(host_path, error) || error)
        {
            write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOTEMPTY);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_truncate(const macos_syscall_context& c)
    {
        std::string guest_path{};
        if (const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path); path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        const auto length = static_cast<int64_t>(get_macos_syscall_argument(c, 1));
        if (length < 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        if (is_read_only_guest_path(c, guest_path))
        {
            write_macos_syscall_error(c, MACOS_EACCES);
            return;
        }

        std::error_code error{};
        std::filesystem::resize_file(resolve_guest_path(c, guest_path), static_cast<uintmax_t>(length), error);
        if (error)
        {
            write_macos_syscall_error(c, map_filesystem_error(error));
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_unlink(const macos_syscall_context& c)
    {
        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        const auto host_path = resolve_guest_path(c, guest_path);

        std::error_code error{};
        const auto link_status = std::filesystem::symlink_status(host_path, error);
        if (link_status.type() == std::filesystem::file_type::not_found)
        {
            write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
            return;
        }

        if (error)
        {
            write_macos_syscall_error(c, map_filesystem_error(error));
            return;
        }

        if (link_status.type() == std::filesystem::file_type::directory)
        {
            write_macos_syscall_error(c, MACOS_EPERM);
            return;
        }

        if (is_read_only_guest_path(c, guest_path))
        {
            write_macos_syscall_error(c, MACOS_EACCES);
            return;
        }

        if (!std::filesystem::remove(host_path, error) || error)
        {
            write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_getdirentries64(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto buffer = get_macos_syscall_argument(c, 1);
        const auto buffer_size = get_macos_syscall_argument(c, 2);
        const auto position_address = get_macos_syscall_argument(c, 3);

        auto* fd_entry = c.proc.fds.get(fd);
        if (fd_entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (fd_entry->type != fd_type::directory)
        {
            write_macos_syscall_error(c, MACOS_ENOTDIR);
            return;
        }

        if (!c.proc.directory_entries.contains(fd))
        {
            c.proc.directory_entries[fd] = read_host_directory(std::filesystem::path{fd_entry->host_path}, c.emu_ref.file_sys.root());
            c.proc.directory_offsets[fd] = 0;
        }

        const auto& entries = c.proc.directory_entries[fd];
        auto& offset = c.proc.directory_offsets[fd];

        uint64_t written = 0;
        std::array<uint8_t, MACOS_DIRENT_MAX_RECORD> record{};

        while (offset < entries.size())
        {
            const auto& item = entries[offset];
            const auto name_length = item.name.size();
            if (name_length > MACOS_PATH_MAX)
            {
                ++offset;
                continue;
            }

            const auto record_length = align_up(MACOS_DIRENT_RECORD_BASE + name_length + 1, MACOS_DIRENT_ALIGNMENT);
            if (record_length > buffer_size || written > buffer_size - record_length)
            {
                break;
            }

            std::fill_n(record.begin(), record_length, uint8_t{0});

            macos_dirent64_header header{};
            header.d_ino = item.inode;
            header.d_seekoff = offset + 1;
            header.d_reclen = static_cast<uint16_t>(record_length);
            header.d_namlen = static_cast<uint16_t>(name_length);
            header.d_type = item.type;

            memcpy(record.data(), &header, sizeof(header));
            memcpy(record.data() + sizeof(header), item.name.data(), name_length);

            if (!c.emu_ref.memory.try_write_memory(buffer + written, record.data(), static_cast<size_t>(record_length)))
            {
                if (written == 0)
                {
                    write_macos_syscall_error(c, MACOS_EFAULT);
                    return;
                }

                break;
            }

            written += record_length;
            ++offset;
        }

        if (position_address != 0)
        {
            const auto position = static_cast<int64_t>(offset);
            if (!c.emu_ref.memory.try_write_memory(position_address, &position, sizeof(position)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }
        }

        write_macos_syscall_result(c, static_cast<int64_t>(written));
    }

    void sys_statfs64(const macos_syscall_context& c)
    {
        std::string guest_path{};
        const auto path_error = read_guest_path(c, get_macos_syscall_argument(c, 0), guest_path);
        if (path_error != 0)
        {
            write_macos_syscall_error(c, path_error);
            return;
        }

        std::error_code error{};
        const auto status = std::filesystem::status(resolve_guest_path(c, guest_path), error);
        if (error || status.type() == std::filesystem::file_type::not_found)
        {
            write_macos_syscall_error(c, error ? map_filesystem_error(error) : MACOS_ENOENT);
            return;
        }

        write_statfs_result(c, get_macos_syscall_argument(c, 1));
    }

    void sys_fstatfs64(const macos_syscall_context& c)
    {
        if (c.proc.fds.get(read_syscall_fd(c, 0)) == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        write_statfs_result(c, get_macos_syscall_argument(c, 1));
    }

}
