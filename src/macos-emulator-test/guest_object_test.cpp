#include <gtest/gtest.h>

#include <guest/guest_memory_object.hpp>
#include <unicorn_arm64_emulator.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace
{
    constexpr uint64_t scratch_base = 0x300000000ULL;
    constexpr size_t scratch_size = 0x4000;

    std::unique_ptr<sogen::arm64_mappable_emulator> make_backend()
    {
        auto emu = sogen::unicorn::create_arm64_emulator();
        emu->map_memory(scratch_base, scratch_size, sogen::memory_permission::read_write);
        return emu;
    }

    TEST(GuestObject, RoundTripsATrivialValue)
    {
        const auto emu = make_backend();
        const sogen::guest_object<uint64_t> object{*emu, scratch_base};

        object.write(0x1122334455667788ULL);
        EXPECT_EQ(object.read(), 0x1122334455667788ULL);
        EXPECT_EQ(object.size(), sizeof(uint64_t));
        EXPECT_EQ(object.end(), scratch_base + sizeof(uint64_t));
    }

    TEST(GuestObject, TryReadFailsOnUnmappedMemory)
    {
        const auto emu = make_backend();
        const sogen::guest_object<uint64_t> object{*emu, 0x900000000ULL};

        EXPECT_FALSE(object.try_read().has_value());
    }

    TEST(GuestAllocator, ReservesAlignedRangesAndRefusesOverflow)
    {
        const auto emu = make_backend();
        sogen::guest_allocator allocator{*emu, scratch_base, 0x100};

        EXPECT_EQ(allocator.reserve(1, 1), scratch_base);
        EXPECT_EQ(allocator.reserve(8, 8), scratch_base + 8);
        EXPECT_THROW((void)allocator.reserve(0x1000, 1), std::runtime_error);
    }

    TEST(ReadGuestString, StopsAtTheTerminator)
    {
        const auto emu = make_backend();
        constexpr std::string_view text = "dyld";
        emu->write_memory(scratch_base, text.data(), text.size() + 1);

        EXPECT_EQ(sogen::read_guest_string<char>(*emu, scratch_base), "dyld");
        EXPECT_EQ(sogen::read_guest_string<char>(*emu, scratch_base, 2u), "dy");
    }
}
