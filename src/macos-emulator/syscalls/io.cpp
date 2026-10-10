#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <cstdio>
#include <string_view>
#include <vector>

namespace sogen
{
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
}
