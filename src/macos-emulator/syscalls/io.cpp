#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MACOS_O_ACCMODE = 0x3;
        constexpr uint64_t MACOS_O_RDONLY = 0x0;
        constexpr uint64_t MACOS_O_RDWR = 0x2;
        constexpr uint64_t MACOS_O_DIRECTORY = 0x100000;

        constexpr uint64_t MACOS_F_DUPFD = 0;
        constexpr uint64_t MACOS_F_GETFD = 1;
        constexpr uint64_t MACOS_F_SETFD = 2;
        constexpr uint64_t MACOS_F_GETFL = 3;
        constexpr uint64_t MACOS_F_DUPFD_CLOEXEC = 67;
        constexpr uint64_t MACOS_FD_CLOEXEC = 1;
        constexpr uint64_t MACOS_F_NOCACHE = 48;
        constexpr uint64_t MACOS_F_GETPATH = 50;
        constexpr size_t MACOS_MAXPATHLEN = 1024;

        constexpr uint64_t WRITE_CHUNK_SIZE = 0x10000;
        constexpr uint64_t READ_CHUNK_SIZE = 0x10000;

        void write_standard_stream(macos_emulator& emu, FILE* stream, const std::string_view data)
        {
            const auto& callback = stream == stdout ? emu.on_stdout : emu.on_stderr;
            if (callback)
            {
                callback(data);
                return;
            }

            fwrite(data.data(), 1, data.size(), stream);
            fflush(stream);
        }

        bool is_output_stream(const posix_fd& file)
        {
            return file.handle == stdout || file.handle == stderr;
        }

        std::optional<int> open_for_reading(posix_fd_table& fds, std::string guest_path, const std::filesystem::path& host_path)
        {
            auto* handle = open_host_file(host_path, "rb");
            if (handle == nullptr)
            {
                return std::nullopt;
            }

            posix_fd file{};
            file.type = fd_type::file;
            file.host_path = utf8_path_string(host_path);
            file.guest_path = std::move(guest_path);
            file.handle = handle;
            file.flags = static_cast<int>(MACOS_O_RDONLY);
            return fds.allocate(std::move(file));
        }

        void duplicate_descriptor(const macos_syscall_context& c, const int fd, const uint64_t minimum_fd, const bool close_on_exec)
        {
            if (minimum_fd >= MACOS_DESCRIPTOR_LIMIT)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
                return;
            }

            const auto new_fd = c.emu_ref.fds.dup_fd(fd, static_cast<int>(minimum_fd));
            if (new_fd < 0 || static_cast<uint64_t>(new_fd) >= MACOS_DESCRIPTOR_LIMIT)
            {
                c.emu_ref.fds.close(new_fd);
                write_macos_syscall_error(c, new_fd < 0 ? macos_errno::MACOS_EBADF : macos_errno::MACOS_EMFILE);
                return;
            }

            c.emu_ref.fds.set_close_on_exec(new_fd, close_on_exec);
            write_macos_syscall_success(c, static_cast<uint64_t>(new_fd));
        }

        std::optional<int32_t> readable_bytes(const posix_fd& file)
        {
            std::error_code ec{};
            const auto size = std::filesystem::file_size(path_from_utf8(file.host_path), ec);
            const auto position = file.seek(0, SEEK_CUR);
            if (ec || !position)
            {
                return std::nullopt;
            }

            return static_cast<int32_t>(std::min<uint64_t>(size - std::min(size, *position), INT32_MAX));
        }

        int open_directory(posix_fd_table& fds, std::string guest_path, const std::filesystem::path& host_path)
        {
            posix_fd directory{};
            directory.type = fd_type::directory;
            directory.host_path = utf8_path_string(host_path);
            directory.guest_path = std::move(guest_path);
            directory.flags = static_cast<int>(MACOS_O_RDONLY);
            return fds.allocate(std::move(directory));
        }

        int open_socket(posix_fd_table& fds)
        {
            posix_fd socket{};
            socket.type = fd_type::socket;
            socket.flags = static_cast<int>(MACOS_O_RDWR);
            return fds.allocate(std::move(socket));
        }
    }

    void sys_write(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = get_macos_syscall_argument(c.emu, 2);

        const auto* file = c.emu_ref.fds.get(fd);
        if (file == nullptr || !is_output_stream(*file))
        {
            // Files are only ever opened read-only and sockets never connect, so XNU would fail the write either way.
            const auto is_socket = file != nullptr && file->type == fd_type::socket;
            write_macos_syscall_error(c, is_socket ? macos_errno::MACOS_ENOTCONN : macos_errno::MACOS_EBADF);
            return;
        }

        std::vector<char> chunk(static_cast<size_t>(std::min(size, WRITE_CHUNK_SIZE)));
        uint64_t written = 0;
        while (written < size)
        {
            const auto chunk_size = static_cast<size_t>(std::min<uint64_t>(size - written, chunk.size()));
            if (!c.emu.try_read_memory(buffer + written, chunk.data(), chunk_size))
            {
                break;
            }

            write_standard_stream(c.emu_ref, file->handle, std::string_view{chunk.data(), chunk_size});
            written += chunk_size;
        }

        if (written == 0 && size != 0)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, written);
    }

    void sys_read(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = get_macos_syscall_argument(c.emu, 2);

        const auto* file = c.emu_ref.fds.get(fd);
        if (file == nullptr || is_output_stream(*file))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        if (file->type == fd_type::directory || file->type == fd_type::socket)
        {
            write_macos_syscall_error(c, file->type == fd_type::directory ? macos_errno::MACOS_EISDIR : macos_errno::MACOS_ENOTCONN);
            return;
        }

        // A stream cannot give back what it delivered, so the whole destination must be writable before anything
        // is taken from the host; XNU only consumes what it copies out.
        std::vector<std::byte> data(static_cast<size_t>(std::min(size, READ_CHUNK_SIZE)));
        if (!c.emu_ref.memory.is_writable_range(buffer, data.size()))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        const auto bytes_read = file->read(data);
        if (!bytes_read)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EIO);
            return;
        }

        if (!c.emu.try_write_memory(buffer, data.data(), *bytes_read))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, *bytes_read);
    }

    void sys_lseek(const macos_syscall_context& c)
    {
        constexpr std::array host_origins{SEEK_SET, SEEK_CUR, SEEK_END};

        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto offset = static_cast<int64_t>(get_macos_syscall_argument(c.emu, 1));
        const auto whence = get_macos_syscall_argument(c.emu, 2);

        const auto* file = c.emu_ref.fds.get(fd);
        if (file == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        if (file->type == fd_type::directory)
        {
            c.emu_ref.log.error("Unhandled lseek on directory %d\n", fd);
            c.emu_ref.stop();
            return;
        }

        if (file->type != fd_type::file || file->is_standard_stream())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ESPIPE);
            return;
        }

        if (whence >= host_origins.size())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto position = file->seek(offset, host_origins.at(static_cast<size_t>(whence)));
        if (!position)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        write_macos_syscall_success(c, *position);
    }

    void sys_open(const macos_syscall_context& c)
    {
        const auto requested_path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        const auto flags = get_macos_syscall_argument(c.emu, 1);

        if (!requested_path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (requested_path->empty())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        const auto guest_path = c.emu_ref.resolve_guest_path(*requested_path);
        if ((flags & MACOS_O_ACCMODE) != MACOS_O_RDONLY)
        {
            c.emu_ref.log.log("open(%s, 0x%llx) -> EROFS\n", guest_path.c_str(), static_cast<unsigned long long>(flags));
            write_macos_syscall_error(c, macos_errno::MACOS_EROFS);
            return;
        }

        const auto host_path = c.emu_ref.file_system.translate(guest_path);
        const auto is_directory = std::filesystem::is_directory(host_path);
        if ((flags & MACOS_O_DIRECTORY) && !is_directory && std::filesystem::exists(host_path))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOTDIR);
            return;
        }

        std::optional<int> fd{};
        if (is_directory)
        {
            fd = open_directory(c.emu_ref.fds, guest_path, host_path);
        }
        else if (std::filesystem::is_regular_file(host_path))
        {
            fd = open_for_reading(c.emu_ref.fds, guest_path, host_path);
        }

        c.emu_ref.log.log("open(%s, 0x%llx) -> %d\n", guest_path.c_str(), static_cast<unsigned long long>(flags), fd.value_or(-1));

        if (!fd)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        write_macos_syscall_success(c, static_cast<uint64_t>(*fd));
    }

    void sys_dup(const macos_syscall_context& c)
    {
        duplicate_descriptor(c, static_cast<int>(get_macos_syscall_argument(c.emu, 0)), 0, false);
    }

    void sys_dup2(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto new_fd = static_cast<int>(get_macos_syscall_argument(c.emu, 1));

        if (c.emu_ref.fds.get(fd) == nullptr || new_fd < 0 || static_cast<uint64_t>(new_fd) >= MACOS_DESCRIPTOR_LIMIT)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        if (c.emu_ref.fds.dup2_fd(fd, new_fd) < 0)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EMFILE);
            return;
        }

        write_macos_syscall_success(c, static_cast<uint64_t>(new_fd));
    }

    void sys_getdtablesize(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, MACOS_DESCRIPTOR_LIMIT);
    }

    void sys_close(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        if (!c.emu_ref.fds.close(fd))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_fcntl(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto command = get_macos_syscall_argument(c.emu, 1);
        const auto argument = get_macos_syscall_argument(c.emu, 2);

        c.emu_ref.log.log("fcntl(%d, %llu, 0x%llx)\n", fd, static_cast<unsigned long long>(command),
                          static_cast<unsigned long long>(argument));

        const auto* file = c.emu_ref.fds.get(fd);
        if (file == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        switch (command)
        {
        case MACOS_F_DUPFD:
        case MACOS_F_DUPFD_CLOEXEC:
            duplicate_descriptor(c, fd, argument, command == MACOS_F_DUPFD_CLOEXEC);
            return;
        case MACOS_F_GETFD:
            write_macos_syscall_success(c, file->close_on_exec ? MACOS_FD_CLOEXEC : 0);
            return;
        case MACOS_F_SETFD:
            c.emu_ref.fds.set_close_on_exec(fd, (argument & MACOS_FD_CLOEXEC) != 0);
            write_macos_syscall_success(c, 0);
            return;
        case MACOS_F_GETFL:
            write_macos_syscall_success(c, static_cast<uint64_t>(file->flags));
            return;
        // XNU's F_GETPATH and F_NOCACHE only apply to vnodes; pipes and sockets get EBADF.
        case MACOS_F_NOCACHE:
            if (!is_vnode(*file))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
                return;
            }

            write_macos_syscall_success(c, 0);
            return;
        case MACOS_F_GETPATH:
            if (!is_vnode(*file))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
                return;
            }

            if (file->guest_path.size() + 1 > MACOS_MAXPATHLEN ||
                !c.emu.try_write_memory(argument, file->guest_path.c_str(), file->guest_path.size() + 1))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
            return;
        default:
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }
    }

    void sys_fsgetpath(const macos_syscall_context& c)
    {
        const auto buffer = get_macos_syscall_argument(c.emu, 0);
        const auto buffer_size = get_macos_syscall_argument(c.emu, 1);
        const auto fsid_pointer = get_macos_syscall_argument(c.emu, 2);

        macos_file_id id{.object_id = get_macos_syscall_argument(c.emu, 3)};
        if (!c.emu.try_read_memory(fsid_pointer, id.fsid.data(), sizeof(id.fsid)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        const auto* path = c.emu_ref.file_ids.find(id);
        if (path == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        const auto length = path->size() + 1;
        if (length > buffer_size || !c.emu.try_write_memory(buffer, path->c_str(), length))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOSPC);
            return;
        }

        write_macos_syscall_success(c, length);
    }

    // POSIX shared memory objects are created by system daemons (notifyd, ...), which do not run in the emulator.
    void sys_shm_open(const macos_syscall_context& c)
    {
        const auto name = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        c.emu_ref.log.log("shm_open(%s) -> ENOENT\n", name ? name->c_str() : "?");
        write_macos_syscall_error(c, name ? macos_errno::MACOS_ENOENT : macos_errno::MACOS_EFAULT);
    }

    // Close-on-exec is handled by the syscall layer for every descriptor. The standard streams then behave like
    // pipes (XNU's pipe_ioctl) and files like vnodes (vn_ioctl): non-blocking, async and readable-byte queries
    // succeed, and everything else is ENOTTY.
    void sys_ioctl(const macos_syscall_context& c)
    {
        constexpr uint64_t fioclex = 0x20006601;
        constexpr uint64_t fionclex = 0x20006602;
        constexpr uint64_t fionread = 0x4004667F;
        constexpr uint64_t fionbio = 0x8004667E;
        constexpr uint64_t fioasync = 0x8004667D;

        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto request = get_macos_syscall_argument(c.emu, 1);
        const auto argument = get_macos_syscall_argument(c.emu, 2);

        const auto* file = c.emu_ref.fds.get(fd);
        if (file == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        const auto is_pipe = file->is_standard_stream();
        const auto is_file = file->type == fd_type::file && !is_pipe;
        if (request == fioclex || request == fionclex || ((is_pipe || is_file) && (request == fionbio || request == fioasync)))
        {
            write_macos_syscall_success(c, 0);
            return;
        }

        if ((is_pipe || is_file) && request == fionread)
        {
            const auto available = is_pipe ? std::optional<int32_t>{0} : readable_bytes(*file);
            if (!available)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EIO);
                return;
            }

            if (!c.emu.try_write_memory(argument, &*available, sizeof(*available)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
            return;
        }

        if (is_pipe || is_file)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOTTY);
            return;
        }

        c.emu_ref.log.error("Unhandled ioctl(%d, 0x%llx)\n", fd, static_cast<unsigned long long>(request));
        c.emu_ref.stop();
    }

    // Local sockets can be created, but no daemon listens anywhere in the emulation root, so every address a
    // guest tries to reach does not exist.
    void sys_socket(const macos_syscall_context& c)
    {
        constexpr uint64_t af_unix = 1;

        const auto domain = get_macos_syscall_argument(c.emu, 0);
        if (domain != af_unix)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EAFNOSUPPORT);
            return;
        }

        write_macos_syscall_success(c, static_cast<uint64_t>(open_socket(c.emu_ref.fds)));
    }

    void sys_connect(const macos_syscall_context& c)
    {
        const auto* file = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (file == nullptr || file->type != fd_type::socket)
        {
            write_macos_syscall_error(c, file == nullptr ? macos_errno::MACOS_EBADF : macos_errno::MACOS_ENOTSOCK);
            return;
        }

        write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
    }

    void sys_sendto(const macos_syscall_context& c)
    {
        const auto* file = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (file == nullptr || file->type != fd_type::socket)
        {
            write_macos_syscall_error(c, file == nullptr ? macos_errno::MACOS_EBADF : macos_errno::MACOS_ENOTSOCK);
            return;
        }

        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = std::min<uint64_t>(get_macos_syscall_argument(c.emu, 2), 512);
        std::string message(static_cast<size_t>(size), '\0');
        if (!c.emu.try_read_memory(buffer, message.data(), message.size()))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        c.emu_ref.log.log("sendto: %s\n", message.c_str());

        const auto address = get_macos_syscall_argument(c.emu, 4);
        write_macos_syscall_error(c, address == 0 ? macos_errno::MACOS_ENOTCONN : macos_errno::MACOS_ENOENT);
    }

    void sys_getpeername(const macos_syscall_context& c)
    {
        const auto* file = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (file == nullptr || file->type != fd_type::socket)
        {
            write_macos_syscall_error(c, file == nullptr ? macos_errno::MACOS_EBADF : macos_errno::MACOS_ENOTSOCK);
            return;
        }

        write_macos_syscall_error(c, macos_errno::MACOS_ENOTCONN);
    }
}
