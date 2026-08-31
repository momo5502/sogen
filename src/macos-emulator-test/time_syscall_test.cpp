#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <chrono>
#include <cstdlib>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    TEST(MacosTimeSyscalls, MachAbsoluteTimeTrapReturnsANonZeroCounter)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0x92800050, // mov x16, #-3
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(2);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
    }

    TEST(MacosTimeSyscalls, MachAbsoluteTimeAdvancesBetweenCalls)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0x92800050, // mov x16, #-3
                                         0xD4001001, // svc #0x80
                                     });
        emu->start(2);
        const auto first = emu->emu().reg(sogen::arm64_register::x0);

        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(2);
        const auto second = emu->emu().reg(sogen::arm64_register::x0);

        EXPECT_GE(second, first);
    }

    TEST(MacosTimeSyscalls, MachContinuousTimeTrapIsDistinctFromAbsolute)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0x92800070, // mov x16, #-4
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(2);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosTimeSyscalls, GettimeofdayFillsAPlausibleWallClock)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {0xD2800E90, 0xD4001001}); // mov x16, #116 (gettimeofday)
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});

        emu->start(2);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        sogen::macos_timeval value{};
        emu->memory.read_memory(data_base, &value, sizeof(value));
        EXPECT_GT(value.tv_sec, 1700000000);
        EXPECT_GE(value.tv_usec, 0);
        EXPECT_LT(value.tv_usec, 1000000);
    }

    TEST(MacosTimeSyscalls, CommpageApproxTimeTracksTheTrap)
    {
        const auto emu = macos_test::make_emulator();

        uint64_t before{};
        emu->memory.read_memory(sogen::MACOS_COMMPAGE_BASE + 0x0C0, &before, sizeof(before));

        macos_test::write_guest_code(*emu, code_base, {0x92800050, 0xD4001001});
        emu->start(2);

        uint64_t after{};
        emu->memory.read_memory(sogen::MACOS_COMMPAGE_BASE + 0x0C0, &after, sizeof(after));
        EXPECT_GE(after, before);
    }

    TEST(MacosTimeSyscalls, MachAbsoluteTimeIsTheVirtualCounterItself)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0x92800050, 0xD4001001});

        const auto before = emu->emu().read_system_register(3, 3, 14, 0, 2);
        emu->start(2);
        const auto after = emu->emu().read_system_register(3, 3, 14, 0, 2);

        const auto reported = emu->emu().reg(sogen::arm64_register::x0);
        EXPECT_GE(reported, before) << "mach_absolute_time must not predate the counter read before the trap";
        EXPECT_LE(reported, after) << "a scaled or rebased counter would overshoot the read taken after the trap";
    }

    TEST(MacosTimeSyscalls, MachAbsoluteTimeAddsTheCommpageTimebaseOffset)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t offset = 0x4000000000000000ULL;
        emu->memory.write_memory(sogen::MACOS_COMMPAGE_BASE + 0x088, &offset, sizeof(offset));

        macos_test::write_guest_code(*emu, code_base, {0x92800050, 0xD4001001});

        const auto counter = emu->emu().read_system_register(3, 3, 14, 0, 2);
        emu->start(2);

        const auto reported = emu->emu().reg(sogen::arm64_register::x0);
        EXPECT_GE(reported, offset + counter) << "TIMEBASE_OFFSET is an addend libSystem applies to the counter it just read";
    }

    TEST(MacosTimeSyscalls, MachContinuousTimeAddsTheContinuousHardwareTimebase)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t continuous = 0x4000000000000000ULL;
        emu->memory.write_memory(sogen::MACOS_COMMPAGE_BASE + 0x0A8, &continuous, sizeof(continuous));

        constexpr uint64_t absolute = 0x1000000000000000ULL;
        emu->memory.write_memory(sogen::MACOS_COMMPAGE_BASE + 0x088, &absolute, sizeof(absolute));

        macos_test::write_guest_code(*emu, code_base, {0x92800070, 0xD4001001});

        const auto counter = emu->emu().read_system_register(3, 3, 14, 0, 2);
        emu->start(2);

        const auto reported = emu->emu().reg(sogen::arm64_register::x0);
        EXPECT_GE(reported, continuous + counter);
        EXPECT_LT(reported, continuous + absolute) << "continuous time must not also carry the absolute-time offset";
    }

    TEST(MacosTimeSyscalls, GettimeofdayReturnsTheAbsoluteTimeInTheSecondRegister)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {0xD2800E90, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});

        const auto before = emu->emu().read_system_register(3, 3, 14, 0, 2);
        emu->start(2);
        const auto after = emu->emu().read_system_register(3, 3, 14, 0, 2);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        const auto counter = emu->emu().reg(sogen::arm64_register::x1);
        EXPECT_GE(counter, before) << "xnu returns the mach absolute time in the second register";
        EXPECT_LE(counter, after);
    }

    TEST(MacosTimeSyscalls, GettimeofdayAgreesWithTheHostWallClock)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {0xD2800E90, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});

        const auto expected = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        emu->start(2);

        sogen::macos_timeval value{};
        emu->memory.read_memory(data_base, &value, sizeof(value));

        EXPECT_LT(std::abs(value.tv_sec - expected), 5) << "the wall clock must be the host's, not the counter's arbitrary epoch";
    }

    TEST(MacosTimeSyscalls, GettimeofdayZeroFillsTheTimezone)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint64_t zone_address = data_base + 0x100;
        constexpr std::array<uint8_t, 8> poison{0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        emu->memory.write_memory(zone_address, poison.data(), poison.size());

        macos_test::write_guest_code(*emu, code_base, {0xD2800E90, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, zone_address);

        emu->start(2);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        std::array<uint8_t, 8> zone{};
        emu->memory.read_memory(zone_address, zone.data(), zone.size());
        EXPECT_EQ(zone, (std::array<uint8_t, 8>{}));
    }

    TEST(MacosTimeSyscalls, GettimeofdayWithAnUnmappedPointerFailsWithEfault)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base, {0xD2800E90, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x700000000000ULL});
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 14u) << "EFAULT";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
    }

    TEST(MacosTimeSyscalls, CommpageApproxTimeIsRefreshedByTheTrap)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t stale = 1;
        emu->memory.write_memory(sogen::MACOS_COMMPAGE_BASE + 0x0C0, &stale, sizeof(stale));

        macos_test::write_guest_code(*emu, code_base, {0x92800050, 0xD4001001});
        emu->start(2);

        uint64_t after{};
        emu->memory.read_memory(sogen::MACOS_COMMPAGE_BASE + 0x0C0, &after, sizeof(after));

        EXPECT_GT(after, stale) << "the trap must republish APPROX_TIME, or a guest reading the commpage sees a frozen clock";
        EXPECT_GE(after, emu->emu().reg(sogen::arm64_register::x0))
            << "APPROX_TIME is written after the counter the trap returned, so it cannot lag it";
    }

    TEST(MacosTimeSyscalls, TheTimevalLayoutMatchesDarwin)
    {
        EXPECT_EQ(sizeof(sogen::macos_timeval), 16u);
        EXPECT_EQ(offsetof(sogen::macos_timeval, tv_sec), 0u);
        EXPECT_EQ(offsetof(sogen::macos_timeval, tv_usec), 8u);
        EXPECT_EQ(sizeof(sogen::macos_timeval{}.tv_sec), 8u);
        EXPECT_EQ(sizeof(sogen::macos_timeval{}.tv_usec), 4u);
    }
}
