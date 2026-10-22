#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MACOS_O_ACCMODE = 0x3;
        constexpr uint64_t MACOS_O_RDONLY = 0x0;

        constexpr uint64_t MACOS_F_GETFD = 1;
        constexpr uint64_t MACOS_F_SETFD = 2;
        constexpr uint64_t MACOS_F_NOCACHE = 48;
        constexpr uint64_t MACOS_F_GETPATH = 50;
        constexpr size_t MACOS_MAXPATHLEN = 1024;
    }

    void sys_write(const macos_syscall_context& c)
    {
        const auto fd = get_macos_syscall_argument(c.emu, 0);
        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = static_cast<size_t>(get_macos_syscall_argument(c.emu, 2));

        if (fd != 1 && fd != 2)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        std::vector<char> data(size);
        if (!c.emu.try_read_memory(buffer, data.data(), size))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        const auto& callback = fd == 1 ? c.emu_ref.on_stdout : c.emu_ref.on_stderr;
        if (callback)
        {
            callback(std::string_view{data.data(), size});
        }
        else
        {
            auto* stream = fd == 1 ? stdout : stderr;
            fwrite(data.data(), 1, size, stream);
            fflush(stream);
        }

        write_macos_syscall_success(c, size);
    }

    void sys_open(const macos_syscall_context& c)
    {
        const auto guest_path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        const auto flags = get_macos_syscall_argument(c.emu, 1);

        if (!guest_path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if ((flags & MACOS_O_ACCMODE) != MACOS_O_RDONLY)
        {
            c.emu_ref.log.log("open(%s, 0x%llx) -> EROFS\n", guest_path->c_str(), static_cast<unsigned long long>(flags));
            write_macos_syscall_error(c, macos_errno::MACOS_EROFS);
            return;
        }

        const auto host_path = c.emu_ref.file_system.translate(*guest_path);
        const auto fd = std::filesystem::is_regular_file(host_path) ? c.emu_ref.fds.open_for_reading(*guest_path, host_path) : std::nullopt;

        c.emu_ref.log.log("open(%s, 0x%llx) -> %d\n", guest_path->c_str(), static_cast<unsigned long long>(flags), fd.value_or(-1));

        if (!fd)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        write_macos_syscall_success(c, static_cast<uint64_t>(*fd));
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
        case MACOS_F_GETFD:
        case MACOS_F_SETFD:
        case MACOS_F_NOCACHE:
            write_macos_syscall_success(c, 0);
            return;
        case MACOS_F_GETPATH:
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

    // The standard streams behave like pipes (XNU's pipe_ioctl): non-blocking, async and readable-byte queries
    // succeed, close-on-exec is handled by the syscall layer, and everything else is ENOTTY.
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

        if (fd < 0 || fd > 2)
        {
            c.emu_ref.log.error("Unhandled ioctl(%d, 0x%llx)\n", fd, static_cast<unsigned long long>(request));
            c.emu_ref.stop();
            return;
        }

        switch (request)
        {
        case fioclex:
        case fionclex:
        case fionbio:
        case fioasync:
            write_macos_syscall_success(c, 0);
            return;
        case fionread: {
            constexpr int32_t available = 0;
            if (!c.emu.try_write_memory(argument, &available, sizeof(available)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
            return;
        }
        default:
            write_macos_syscall_error(c, macos_errno::MACOS_ENOTTY);
            return;
        }
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

        write_macos_syscall_success(c, static_cast<uint64_t>(c.emu_ref.fds.open_socket()));
    }

    void sys_connect(const macos_syscall_context& c)
    {
        const auto* file = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (file == nullptr || !file->is_socket)
        {
            write_macos_syscall_error(c, file == nullptr ? macos_errno::MACOS_EBADF : macos_errno::MACOS_ENOTSOCK);
            return;
        }

        write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
    }

    void sys_sendto(const macos_syscall_context& c)
    {
        const auto* file = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (file == nullptr || !file->is_socket)
        {
            write_macos_syscall_error(c, file == nullptr ? macos_errno::MACOS_EBADF : macos_errno::MACOS_ENOTSOCK);
            return;
        }

        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = std::min<uint64_t>(get_macos_syscall_argument(c.emu, 2), 512);
        std::string message(static_cast<size_t>(size), '\0');
        c.emu.try_read_memory(buffer, message.data(), message.size());
        c.emu_ref.log.log("sendto: %s\n", message.c_str());

        const auto address = get_macos_syscall_argument(c.emu, 4);
        write_macos_syscall_error(c, address == 0 ? macos_errno::MACOS_ENOTCONN : macos_errno::MACOS_ENOENT);
    }
}
