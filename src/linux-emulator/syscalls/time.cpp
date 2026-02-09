#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

using namespace linux_errno;

namespace
{
    // Linux clock IDs (for reference):
    // 0 = CLOCK_REALTIME, 1 = CLOCK_MONOTONIC, 2 = CLOCK_PROCESS_CPUTIME_ID,
    // 3 = CLOCK_THREAD_CPUTIME_ID, 4 = CLOCK_MONOTONIC_RAW,
    // 5 = CLOCK_REALTIME_COARSE, 6 = CLOCK_MONOTONIC_COARSE, 7 = CLOCK_BOOTTIME

    struct linux_timespec
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct linux_timeval
    {
        int64_t tv_sec;
        int64_t tv_usec;
    };
}

void sys_clock_gettime(const linux_syscall_context& c)
{
    const auto clock_id = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto tp_addr = get_linux_syscall_argument(c.emu, 1);

    (void)clock_id; // We treat all clocks the same for now

    const auto now = std::chrono::system_clock::now();
    const auto duration = now.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto nsecs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration) - std::chrono::duration_cast<std::chrono::nanoseconds>(secs);

    linux_timespec ts{};
    ts.tv_sec = secs.count();
    ts.tv_nsec = nsecs.count();

    c.emu.write_memory(tp_addr, &ts, sizeof(ts));
    write_linux_syscall_result(c, 0);
}

void sys_gettimeofday(const linux_syscall_context& c)
{
    const auto tv_addr = get_linux_syscall_argument(c.emu, 0);
    // tz_addr is arg 1, typically NULL

    const auto now = std::chrono::system_clock::now();
    const auto duration = now.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto usecs =
        std::chrono::duration_cast<std::chrono::microseconds>(duration) - std::chrono::duration_cast<std::chrono::microseconds>(secs);

    linux_timeval tv{};
    tv.tv_sec = secs.count();
    tv.tv_usec = usecs.count();

    if (tv_addr != 0)
    {
        c.emu.write_memory(tv_addr, &tv, sizeof(tv));
    }

    write_linux_syscall_result(c, 0);
}

// --- Phase 4b: Additional time syscalls ---

void sys_nanosleep(const linux_syscall_context& c)
{
    const auto req_addr = get_linux_syscall_argument(c.emu, 0);
    const auto rem_addr = get_linux_syscall_argument(c.emu, 1);

    (void)req_addr; // We don't actually sleep in emulation

    // If rem (remaining) is requested, write zero (no time remaining)
    if (rem_addr != 0)
    {
        linux_timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        c.emu.write_memory(rem_addr, &ts, sizeof(ts));
    }

    write_linux_syscall_result(c, 0);
}

void sys_clock_nanosleep(const linux_syscall_context& c)
{
    // clock_id is arg 0 (ignored)
    // flags is arg 1 (ignored — TIMER_ABSTIME etc.)
    const auto rem_addr = get_linux_syscall_argument(c.emu, 3);

    // If rem (remaining) is requested, write zero
    if (rem_addr != 0)
    {
        linux_timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        c.emu.write_memory(rem_addr, &ts, sizeof(ts));
    }

    write_linux_syscall_result(c, 0);
}
