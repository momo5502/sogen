#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr uint64_t O_ACCMODE = 0x3;
        constexpr uint64_t O_RDONLY = 0x0;

        constexpr uint64_t F_GETFD = 1;
        constexpr uint64_t F_SETFD = 2;
        constexpr uint64_t F_NOCACHE = 48;
        constexpr uint64_t F_GETPATH = 50;
        constexpr size_t MAXPATHLEN = 1024;
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

        if ((flags & O_ACCMODE) != O_RDONLY)
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
        case F_GETFD:
        case F_SETFD:
        case F_NOCACHE:
            write_macos_syscall_success(c, 0);
            return;
        case F_GETPATH:
            if (file->guest_path.size() + 1 > MAXPATHLEN ||
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
}
