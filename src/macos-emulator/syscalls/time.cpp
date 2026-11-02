#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <array>

namespace sogen
{
    void sys_gettimeofday(const macos_syscall_context& c)
    {
        struct macos_timeval
        {
            int64_t seconds;
            int32_t microseconds;
            int32_t padding;
        };

        const auto time_pointer = get_macos_syscall_argument(c.emu, 0);
        const auto timezone_pointer = get_macos_syscall_argument(c.emu, 1);
        const auto absolute_time_pointer = get_macos_syscall_argument(c.emu, 2);

        const macos_timeval now{.seconds = MACOS_EPOCH_SECONDS, .microseconds = 0, .padding = 0};
        constexpr std::array<int32_t, 2> utc_timezone{};
        constexpr uint64_t absolute_time = 0;

        if ((time_pointer != 0 && !c.emu.try_write_memory(time_pointer, &now, sizeof(now))) ||
            (timezone_pointer != 0 && !c.emu.try_write_memory(timezone_pointer, utc_timezone.data(), sizeof(utc_timezone))) ||
            (absolute_time_pointer != 0 && !c.emu.try_write_memory(absolute_time_pointer, &absolute_time, sizeof(absolute_time))))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}
