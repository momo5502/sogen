#include "../std_include.hpp"
#include "../macos_emulator.hpp"
#include "../macos_syscall_utils.hpp"

#include <algorithm>
#include <chrono>

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_errno;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        constexpr int64_t MICROSECONDS_PER_SECOND = 1000000;

        // libSystem's inline mach_absolute_time() reads CNTVCT_EL0, so the trap has to read the same
        // register: answering from CNTPCT_EL0 would let time jump whenever the guest switched between
        // the commpage path and this one.
        uint64_t read_virtual_counter(const macos_syscall_context& c)
        {
            return c.emu.read_system_register(3, 3, 14, 0, 2);
        }

        // TIMEBASE_OFFSET, CONT_TIMEBASE and CONT_HW_TIMEBASE are addends libSystem applies to the
        // counter it just read, not snapshots of it, so the trap adds the same field the inline path
        // would have added.
        uint64_t read_commpage_addend(const macos_syscall_context& c, const uint64_t offset)
        {
            if (!c.emu_ref.commpage.is_mapped())
            {
                return 0;
            }

            uint64_t addend{};
            if (!c.emu_ref.memory.try_read_memory(c.emu_ref.commpage.get_base() + offset, &addend, sizeof(addend)))
            {
                return 0;
            }

            return addend;
        }

        uint64_t current_absolute_time(const macos_syscall_context& c)
        {
            return read_virtual_counter(c) + read_commpage_addend(c, commpage_offset::TIMEBASE_OFFSET);
        }
    }

    void mach_trap_absolute_time(const macos_syscall_context& c)
    {
        const auto now = current_absolute_time(c);

        c.emu_ref.commpage.update_time(c.emu_ref.memory, c.emu);

        write_macos_syscall_result_pair(c, now, 0);
    }

    // The emulated machine never sleeps, so continuous time differs from absolute time only by the
    // addend the commpage publishes. CONT_HW_TIMEBASE rather than CONT_TIMEBASE is the one libSystem
    // uses, because the commpage advertises CONT_HWCLOCK.
    void mach_trap_continuous_time(const macos_syscall_context& c)
    {
        const auto now = read_virtual_counter(c) + read_commpage_addend(c, commpage_offset::CONT_HW_TIMEBASE);

        c.emu_ref.commpage.update_time(c.emu_ref.memory, c.emu);

        write_macos_syscall_result_pair(c, now, 0);
    }

    // xnu hands the caller the mach absolute time in the second return register so that libc can seed
    // its own cache without a second trap, and also copies it out to the third argument when the caller
    // passes one. mach_get_times() reads only the copied-out word and executes brk #1 when it is still
    // zero (measured 2026-08-28: libsystem_kernel.dylib`mach_get_times+0x6c..+0xe4), so the register
    // alone is not enough.
    void sys_gettimeofday(const macos_syscall_context& c)
    {
        const auto time_address = get_macos_syscall_argument(c, 0);
        const auto zone_address = get_macos_syscall_argument(c, 1);
        const auto mach_time_address = get_macos_syscall_argument(c, 2);

        const auto epoch = std::chrono::system_clock::now().time_since_epoch();
        const auto microseconds = std::max<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(epoch).count(), 0);

        if (time_address != 0)
        {
            const macos_timeval value{
                .tv_sec = microseconds / MICROSECONDS_PER_SECOND,
                .tv_usec = static_cast<int32_t>(microseconds % MICROSECONDS_PER_SECOND),
                .tv_pad = 0,
            };

            if (!c.emu_ref.memory.try_write_memory(time_address, &value, sizeof(value)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }
        }

        // struct timezone is obsolete on Darwin and the kernel has reported it as all-zero since
        // 10.4; a guest that passes one still expects the two ints to be written.
        if (zone_address != 0)
        {
            constexpr std::array<int32_t, 2> zone{};
            if (!c.emu_ref.memory.try_write_memory(zone_address, zone.data(), sizeof(zone)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }
        }

        const auto now = current_absolute_time(c);

        if (mach_time_address != 0 && !c.emu_ref.memory.try_write_memory(mach_time_address, &now, sizeof(now)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result_pair(c, 0, now);
    }

}
