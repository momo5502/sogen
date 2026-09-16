#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_types.hpp>
#include <guest/guest_memory_object.hpp>

namespace
{
    constexpr uint64_t scratch = 0x340000000ULL;

    // Unicorn caches translated blocks, so a second program written over the first at one address
    // silently re-executes the first. Every trap invocation gets its own page.
    uint64_t run_trap(sogen::macos_emulator& emu, const uint64_t page, const std::vector<uint32_t>& words)
    {
        emu.memory.allocate_memory(page, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
        macos_test::write_guest_code(emu, page, words);
        emu.start(words.size());
        return emu.emu().reg(sogen::arm64_register::x0);
    }

    TEST(MachObjects, TimebaseTrapWritesNumerAndDenom)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        std::vector<uint32_t> words{};
        macos_test::load_x(words, 0, scratch);
        words.push_back(0x92800B10); // mov x16, #-89
        words.push_back(0xD4001001); // svc #0x80

        EXPECT_EQ(run_trap(*emu, 0x100000000ULL, words), 0u);

        const sogen::guest_object<uint32_t> numer{emu->memory, scratch};
        const sogen::guest_object<uint32_t> denom{emu->memory, scratch + 4};
        EXPECT_EQ(numer.read(), emu->mach.timebase_numer);
        EXPECT_EQ(denom.read(), emu->mach.timebase_denom);
        EXPECT_NE(denom.read(), 0u) << "a zero denominator divides by zero inside libplatform";
    }

    // The emulated CNTFRQ_EL0 is 62.5 MHz (QEMU's GTIMER_SCALE of 16), not Apple hardware's 24 MHz, so
    // the ratio has to be 16/1 rather than 125/3. Reporting Apple's would make every duration a guest
    // derives from mach_absolute_time() wrong by a factor of 2.6.
    TEST(MachObjects, TimebaseMatchesTheEmulatedCounterNotAppleHardware)
    {
        const auto emu = macos_test::make_emulator();

        const auto nanoseconds_per_second = uint64_t{1000000000};
        const auto frequency = emu->emu().read_system_register(3, 3, 14, 0, 0);

        ASSERT_NE(frequency, 0u);
        ASSERT_NE(emu->mach.timebase_denom, 0u);

        const auto reported = (uint64_t{emu->mach.timebase_numer} * frequency) / emu->mach.timebase_denom;
        EXPECT_EQ(reported, nanoseconds_per_second) << "numer/denom must be nanoseconds per tick for the counter the guest actually reads";
    }

    TEST(MachObjects, VoucherTrapReturnsALivePortName)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        std::vector<uint32_t> words{};
        macos_test::load_x(words, 0, emu->mach.host_self);
        macos_test::load_x(words, 1, 0);
        macos_test::load_x(words, 2, 0);
        macos_test::load_x(words, 3, scratch);
        words.push_back(0x928008B0); // mov x16, #-70
        words.push_back(0xD4001001); // svc #0x80

        EXPECT_EQ(run_trap(*emu, 0x100010000ULL, words), 0u);

        const sogen::guest_object<uint32_t> out{emu->memory, scratch};
        EXPECT_NE(out.read(), sogen::mach::PORT_NULL);
        EXPECT_EQ(emu->mach.ports.object_of(out.read()).kind, sogen::mach::kernel_object_kind::voucher);
    }

    TEST(MachObjects, SemaphoreCountsDownAndRefusesToBlock)
    {
        const auto emu = macos_test::make_emulator();
        const auto name = emu->mach.create_semaphore(0, 1);

        ASSERT_NE(name, sogen::mach::PORT_NULL);
        EXPECT_EQ(emu->mach.semaphore_wait(name), sogen::mach::kr::success);
        EXPECT_EQ(emu->mach.semaphore_wait(name), sogen::mach::kr::operation_timed_out)
            << "a single-threaded emulator must fail a would-block wait rather than spin";
        EXPECT_EQ(emu->mach.semaphore_signal(name), sogen::mach::kr::success);
        EXPECT_EQ(emu->mach.semaphore_wait(name), sogen::mach::kr::success);
        EXPECT_EQ(emu->mach.semaphore_wait(0x9999), sogen::mach::kr::invalid_name);
    }

    TEST(MachObjects, ClockServicePortsAreStablePerClockId)
    {
        const auto emu = macos_test::make_emulator();

        const auto system_clock = emu->mach.clock_service(sogen::mach::SYSTEM_CLOCK);
        const auto calendar_clock = emu->mach.clock_service(sogen::mach::CALENDAR_CLOCK);

        EXPECT_NE(system_clock, sogen::mach::PORT_NULL);
        EXPECT_NE(system_clock, calendar_clock);
        EXPECT_EQ(emu->mach.clock_service(sogen::mach::SYSTEM_CLOCK), system_clock);
        EXPECT_EQ(emu->mach.ports.object_of(system_clock).kind, sogen::mach::kernel_object_kind::clock);
    }
}
