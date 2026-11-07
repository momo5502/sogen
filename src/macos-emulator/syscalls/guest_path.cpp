#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <posix_path_syscalls.hpp>

namespace sogen
{
    std::variant<std::string, posix_errno> read_guest_path_argument(const macos_syscall_context& c, const size_t index,
                                                                    const int directory_fd)
    {
        const auto requested_path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, index));
        if (!requested_path)
        {
            return posix_errno::efault;
        }

        const auto directory = directory_fd == MACOS_AT_FDCWD ? std::nullopt : std::optional{directory_fd};
        return posix_resolve_path_at(c.emu_ref.fds, c.emu_ref.working_directory, directory, *requested_path);
    }
}
