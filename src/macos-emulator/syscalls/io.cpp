#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <posix_descriptor_syscalls.hpp>
#include <posix_io_syscalls.hpp>
#include <posix_ioctl.hpp>
#include <posix_open.hpp>
#include <posix_pipe.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr int MACOS_O_ACCMODE = 0x3;
        constexpr int MACOS_O_RDWR = 0x2;
        constexpr int MACOS_O_NONBLOCK = 0x4;
        constexpr int MACOS_O_APPEND = 0x8;
        constexpr int MACOS_O_CREAT = 0x200;
        constexpr int MACOS_O_TRUNC = 0x400;
        constexpr int MACOS_O_EXCL = 0x800;
        constexpr int MACOS_O_DIRECTORY = 0x100000;
        constexpr int MACOS_O_CLOEXEC = 0x1000000;

        // What F_GETFL reports back: the access mode and the status flags, without the flags that only act on open.
        constexpr int MACOS_DESCRIPTOR_STATUS_FLAGS = MACOS_O_ACCMODE | MACOS_O_NONBLOCK | MACOS_O_APPEND;

        constexpr uint64_t MACOS_F_DUPFD = 0;
        constexpr uint64_t MACOS_F_GETFD = 1;
        constexpr uint64_t MACOS_F_SETFD = 2;
        constexpr uint64_t MACOS_F_GETFL = 3;
        constexpr uint64_t MACOS_F_DUPFD_CLOEXEC = 67;
        constexpr uint64_t MACOS_F_NOCACHE = 48;
        constexpr uint64_t MACOS_F_GETPATH = 50;
        constexpr size_t MACOS_MAXPATHLEN = 1024;

        posix_io_context io_context(macos_emulator& emu)
        {
            return {
                .fds = emu.fds,
                .memory = emu.memory,
                .on_stdout = emu.on_stdout,
                .on_stderr = emu.on_stderr,
                .nonblock_flag = MACOS_O_NONBLOCK,
            };
        }

        std::optional<posix_file_ioctl> file_ioctl(const uint64_t request)
        {
            constexpr uint64_t fioclex = 0x20006601;
            constexpr uint64_t fionclex = 0x20006602;
            constexpr uint64_t fionread = 0x4004667F;
            constexpr uint64_t fionbio = 0x8004667E;
            constexpr uint64_t fioasync = 0x8004667D;

            switch (request)
            {
            case fioclex:
                return posix_file_ioctl::set_close_on_exec;
            case fionclex:
                return posix_file_ioctl::clear_close_on_exec;
            case fionread:
                return posix_file_ioctl::read_queued_bytes;
            case fionbio:
                return posix_file_ioctl::set_nonblocking;
            case fioasync:
                return posix_file_ioctl::set_async;
            default:
                return std::nullopt;
            }
        }

        bool takes_file_ioctl(const posix_fd& file, const posix_file_ioctl request)
        {
            switch (request)
            {
            case posix_file_ioctl::set_close_on_exec:
            case posix_file_ioctl::clear_close_on_exec:
            case posix_file_ioctl::set_nonblocking:
                return true;
            case posix_file_ioctl::set_async:
            case posix_file_ioctl::read_queued_bytes:
                return posix_has_file_ioctls(file);
            }

            return false;
        }

        int open_socket(posix_fd_table& fds)
        {
            posix_fd socket{};
            socket.type = fd_type::socket;
            socket.flags = MACOS_O_RDWR;
            return fds.allocate(std::move(socket));
        }
    }

    void sys_write(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = get_macos_syscall_argument(c.emu, 2);
        write_macos_syscall_result(c, posix_write(io_context(c.emu_ref), fd, buffer, size));
    }

    void sys_read(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = get_macos_syscall_argument(c.emu, 2);
        write_macos_syscall_result(c, posix_read(io_context(c.emu_ref), fd, buffer, size));
    }

    void sys_lseek(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto offset = static_cast<int64_t>(get_macos_syscall_argument(c.emu, 1));
        const auto whence = static_cast<uint32_t>(get_macos_syscall_argument(c.emu, 2));
        write_macos_syscall_result(c, posix_lseek(c.emu_ref.fds, fd, offset, whence));
    }

    void sys_open(const macos_syscall_context& c)
    {
        const auto requested_path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        const auto flags = static_cast<int>(get_macos_syscall_argument(c.emu, 1));
        const auto mode = static_cast<uint32_t>(get_macos_syscall_argument(c.emu, 2));

        if (!requested_path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        const auto access = posix_access_mode_from_flags(flags);
        if (!access)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        if (requested_path->empty())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        const posix_open_request request{
            .guest_path = c.emu_ref.resolve_guest_path(*requested_path),
            .access = *access,
            .create = (flags & MACOS_O_CREAT) != 0,
            .exclusive = (flags & MACOS_O_EXCL) != 0,
            .truncate = (flags & MACOS_O_TRUNC) != 0,
            .append = (flags & MACOS_O_APPEND) != 0,
            .directory = (flags & MACOS_O_DIRECTORY) != 0,
            .close_on_exec = (flags & MACOS_O_CLOEXEC) != 0,
            .creation_mode = mode,
            .guest_flags = flags & MACOS_DESCRIPTOR_STATUS_FLAGS,
        };

        const auto result = posix_open(c.emu_ref.fds, c.emu_ref.file_system, request);
        const auto* fd = std::get_if<uint64_t>(&result);
        c.emu_ref.log.log("open(%s, 0x%x) -> %lld\n", request.guest_path.c_str(), static_cast<unsigned>(flags),
                          fd != nullptr ? static_cast<long long>(*fd) : -1LL);
        write_macos_syscall_result(c, result);
    }

    void sys_dup(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        write_macos_syscall_result(c, posix_dup(c.emu_ref.fds, fd, 0, false, MACOS_DESCRIPTOR_LIMIT));
    }

    void sys_dup2(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto new_fd = static_cast<int>(get_macos_syscall_argument(c.emu, 1));
        write_macos_syscall_result(c, posix_dup2(c.emu_ref.fds, fd, new_fd, MACOS_DESCRIPTOR_LIMIT));
    }

    // XNU returns both ends in registers; libSystem's pipe() stores them into the caller's array.
    void sys_pipe(const macos_syscall_context& c)
    {
        const auto result = posix_create_pipe(c.emu_ref.fds, 0, false);
        if (const auto* error = std::get_if<posix_errno>(&result))
        {
            write_macos_syscall_error(c, macos_errno_of(*error));
            return;
        }

        const auto& created = std::get<posix_pipe>(result);
        write_macos_syscall_success(c, static_cast<uint64_t>(created.read_fd));
        c.emu.reg(x86_register::rdx, static_cast<uint64_t>(created.write_fd));
    }

    void sys_getdtablesize(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, MACOS_DESCRIPTOR_LIMIT);
    }

    void sys_close(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        write_macos_syscall_result(c, posix_close(c.emu_ref.fds, fd));
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
            write_macos_syscall_result(
                c, posix_dup(c.emu_ref.fds, fd, static_cast<int>(argument), command == MACOS_F_DUPFD_CLOEXEC, MACOS_DESCRIPTOR_LIMIT));
            return;
        case MACOS_F_GETFD:
            write_macos_syscall_result(c, posix_get_descriptor_flags(c.emu_ref.fds, fd));
            return;
        case MACOS_F_SETFD:
            write_macos_syscall_result(c, posix_set_descriptor_flags(c.emu_ref.fds, fd, argument));
            return;
        case MACOS_F_GETFL:
            write_macos_syscall_result(c, posix_get_status_flags(c.emu_ref.fds, fd));
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

    // XNU's ioctl() sets close-on-exec and non-blocking mode for every descriptor. Pipes and the standard streams then
    // behave as its pipe_ioctl and files as its vn_ioctl: async and readable-byte queries succeed, and everything else
    // is ENOTTY.
    void sys_ioctl(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto request = get_macos_syscall_argument(c.emu, 1);
        const auto argument = get_macos_syscall_argument(c.emu, 2);

        const auto* file = c.emu_ref.fds.get(fd);
        if (file == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        const auto file_request = file_ioctl(request);
        if (file_request && takes_file_ioctl(*file, *file_request))
        {
            write_macos_syscall_result(c, posix_ioctl(io_context(c.emu_ref), fd, *file_request, argument));
            return;
        }

        if (posix_has_file_ioctls(*file))
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
