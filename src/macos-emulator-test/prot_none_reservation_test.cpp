#include <gtest/gtest.h>

#include <macos_memory_report.hpp>

#include "macos_test_utils.hpp"

namespace
{
    constexpr uint64_t reservation_base = 0x400000000ULL;
    constexpr size_t reservation_size = 0x40000000ULL;

    // libmalloc reserves its magazine ranges by mapping tens of gigabytes PROT_NONE and protecting
    // slices to read-write as it needs them. XNU charges nothing for that: a VM_PROT_NONE entry has no
    // resident pages and no swap reservation. Committing it instead is invisible on a host with lazy
    // paging, and fatal under wasm, where linear memory has to be real.
    TEST(ProtNoneReservation, ProtNoneMappingCommitsNothing)
    {
        const auto emu = macos_test::make_emulator();
        const auto before = sogen::collect_macos_memory_report(emu->memory);

        ASSERT_TRUE(emu->memory.allocate_memory(reservation_base, reservation_size, sogen::memory_permission::none));

        const auto after = sogen::collect_macos_memory_report(emu->memory);

        EXPECT_EQ(after.guest_committed_bytes, before.guest_committed_bytes) << "a PROT_NONE mapping must not commit memory";
        EXPECT_EQ(after.guest_reserved_bytes - before.guest_reserved_bytes, reservation_size);
        EXPECT_EQ(after.guest_region_count - before.guest_region_count, 1ULL);
    }

    TEST(ProtNoneReservation, ProtNoneMappingIsNotAccessible)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(reservation_base, reservation_size, sogen::memory_permission::none));

        uint32_t value = 0;
        EXPECT_FALSE(emu->memory.try_read_memory(reservation_base, &value, sizeof(value)))
            << "the guest must fault on a reservation it has not protected yet";
    }

    // The half that makes the reservation usable: libmalloc protects a slice to read-write and expects
    // to be able to use it. A reservation that could never be promoted would be worse than committing.
    TEST(ProtNoneReservation, ProtectingAReservationBacksIt)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(reservation_base, reservation_size, sogen::memory_permission::none));

        constexpr size_t slice = 0x8000;
        ASSERT_TRUE(emu->memory.protect_memory(reservation_base, slice, sogen::memory_permission::read_write));

        constexpr uint32_t written = 0xA5A5A5A5;
        ASSERT_TRUE(emu->memory.try_write_memory(reservation_base, &written, sizeof(written)));

        uint32_t read_back = 0;
        ASSERT_TRUE(emu->memory.try_read_memory(reservation_base, &read_back, sizeof(read_back)));
        EXPECT_EQ(read_back, written);
    }

    TEST(ProtNoneReservation, PromotingASliceLeavesTheRestReserved)
    {
        const auto emu = macos_test::make_emulator();
        const auto before = sogen::collect_macos_memory_report(emu->memory);

        ASSERT_TRUE(emu->memory.allocate_memory(reservation_base, reservation_size, sogen::memory_permission::none));

        constexpr size_t slice = 0x8000;
        ASSERT_TRUE(emu->memory.protect_memory(reservation_base, slice, sogen::memory_permission::read_write));

        const auto after = sogen::collect_macos_memory_report(emu->memory);

        EXPECT_EQ(after.guest_committed_bytes - before.guest_committed_bytes, slice) << "only the promoted slice is committed";
        EXPECT_EQ(after.guest_reserved_bytes - before.guest_reserved_bytes, reservation_size);

        uint32_t value = 0;
        EXPECT_FALSE(emu->memory.try_read_memory(reservation_base + slice, &value, sizeof(value)))
            << "the untouched remainder is still a reservation";
    }
}
