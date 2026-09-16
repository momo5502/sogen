#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <array>
#include <chrono>
#include <commpage.hpp>
#include <macos_memory_manager.hpp>

namespace
{
    constexpr uint64_t commpage_base = 0xFFFFFC000ULL;
    constexpr uint64_t commpage_ro_base = 0xFFFFF4000ULL;

    struct commpage_fixture
    {
        std::unique_ptr<sogen::arm64_mappable_emulator> backend{macos_test::make_backend()};
        sogen::macos_memory_manager memory{*backend};
        sogen::macos_commpage commpage{};
        sogen::macos_system_info info{};

        commpage_fixture()
        {
            this->commpage.setup(this->memory, *this->backend, this->info);
        }

        template <typename T>
        T read(const uint64_t address) const
        {
            T value{};
            this->memory.read_memory(address, &value, sizeof(value));
            return value;
        }
    };

    TEST(MacosCommpage, MapsBothPagesAtTheDarwinAddresses)
    {
        commpage_fixture fixture{};

        EXPECT_TRUE(fixture.commpage.is_mapped());
        EXPECT_EQ(fixture.commpage.get_base(), commpage_base);
        EXPECT_EQ(fixture.commpage.get_readonly_base(), commpage_ro_base);
        EXPECT_TRUE(fixture.memory.get_region_info(commpage_base).has_value());
        EXPECT_TRUE(fixture.memory.get_region_info(commpage_ro_base).has_value());
    }

    TEST(MacosCommpage, CarriesTheSixtyFourBitSignature)
    {
        commpage_fixture fixture{};

        std::array<char, 16> signature{};
        fixture.memory.read_memory(commpage_base, signature.data(), signature.size());
        EXPECT_STREQ(signature.data(), "commpage 64-bit");
    }

    TEST(MacosCommpage, ReportsCapabilitiesVersionAndCpuCounts)
    {
        commpage_fixture fixture{};

        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x010), fixture.info.cpu_capabilities64);
        EXPECT_EQ(fixture.read<uint16_t>(commpage_base + 0x01E), 3u);
        EXPECT_EQ(fixture.read<uint32_t>(commpage_base + 0x020), fixture.info.cpu_capabilities32);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x022), fixture.info.ncpus);
        EXPECT_EQ(fixture.read<uint16_t>(commpage_base + 0x026), 128u);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x02F), fixture.info.cpu_clusters);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x034), fixture.info.active_cpus);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x035), fixture.info.physical_cpus);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x036), fixture.info.logical_cpus);
        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x038), fixture.info.memory_size);
        EXPECT_EQ(fixture.read<uint32_t>(commpage_base + 0x080), 0x72015832U);
    }

    TEST(MacosCommpage, AdvertisesPointerAuthentication)
    {
        commpage_fixture fixture{};

        const auto capabilities64 = fixture.read<uint64_t>(commpage_base + 0x010);
        EXPECT_NE(capabilities64 & 0x0008000000000000ULL, 0u) << "kHasFeatPAuth must be set";
        EXPECT_NE(capabilities64 & 0x0000000002000000ULL, 0u) << "kHasFeatLSE must be set";
    }

    TEST(MacosCommpage, PageShiftsLiveOnTheReadOnlyPageOnly)
    {
        commpage_fixture fixture{};

        EXPECT_EQ(fixture.read<uint8_t>(commpage_ro_base + 0x024), 14u);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_ro_base + 0x025), 14u);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_ro_base + 0x037), 14u);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x025), 0u);
    }

    TEST(MacosCommpage, CarriesNoTimebaseNumerDenomPair)
    {
        commpage_fixture fixture{};

        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x091), 1u);
        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x088), 0u);
        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x098), 0u);
        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x0A8), 0u);
        EXPECT_NE(fixture.read<uint64_t>(commpage_base + 0x0C0), 0u);
        EXPECT_EQ(fixture.read<uint8_t>(commpage_base + 0x0C8), 1u);
    }

    // libSystem branches on USER_TIMEBASE to choose the counter register it reads inline. Value 2 selects
    // CNTVCTSS_EL0 (S3_3_C14_C0_6) and value 3 the Apple IMPDEF S3_4_C15_C10_6; the emulated CPU raises
    // an exception on both, which would take down every mach_absolute_time() call in the guest.
    TEST(MacosCommpage, AdvertisedUserTimebaseIsExecutableByTheBackend)
    {
        commpage_fixture fixture{};

        ASSERT_EQ(fixture.read<uint8_t>(commpage_base + 0x090), 1u);

        constexpr uint64_t code_base = 0x100000000ULL;
        ASSERT_TRUE(fixture.memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr auto code = std::to_array<uint32_t>({
            0xD5033FDF, // isb
            0xD53BE040, // mrs x0, cntvct_el0
        });
        fixture.memory.write_memory(code_base, code.data(), sizeof(code));
        fixture.backend->reg(sogen::arm64_register::pc, code_base);
        fixture.backend->start(2);

        EXPECT_GE(fixture.backend->reg(sogen::arm64_register::x0), fixture.read<uint64_t>(commpage_base + 0x0C0));
    }

    TEST(MacosCommpage, BoottimeIsCalibratedAgainstTheWallClock)
    {
        commpage_fixture fixture{};

        const auto boottime_usec = fixture.read<uint64_t>(commpage_base + 0x0A0);
        const auto wall_usec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

        EXPECT_NE(boottime_usec, 0u);
        EXPECT_LE(boottime_usec, wall_usec);
        EXPECT_GT(boottime_usec + static_cast<uint64_t>(std::chrono::microseconds(std::chrono::hours(24 * 365)).count()), wall_usec)
            << "the boot instant has to be calibrated, not derived from an epoch-less counter";
    }

    TEST(MacosCommpage, UpdateTimeAdvancesTheApproximateTime)
    {
        commpage_fixture fixture{};

        const auto before = fixture.read<uint64_t>(commpage_base + 0x0C0);
        fixture.commpage.update_time(fixture.memory, *fixture.backend);
        const auto after = fixture.read<uint64_t>(commpage_base + 0x0C0);

        EXPECT_GE(after, before);
        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x098), 0u);
        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x0A8), 0u);
    }

    TEST(MacosCommpage, SurvivesASerializationRoundTrip)
    {
        commpage_fixture fixture{};

        sogen::utils::buffer_serializer serializer{};
        fixture.commpage.serialize(serializer);

        sogen::macos_commpage restored{};
        sogen::utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        EXPECT_TRUE(restored.is_mapped());
        EXPECT_EQ(restored.get_base(), fixture.commpage.get_base());
        EXPECT_EQ(restored.get_readonly_base(), fixture.commpage.get_readonly_base());
    }

    TEST(MacosCommpage, GuestCodeCanReadTheFixedAddresses)
    {
        commpage_fixture fixture{};

        constexpr uint64_t code_base = 0x100000000ULL;
        ASSERT_TRUE(fixture.memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr auto code = std::to_array<uint32_t>({
            0xD2980001, // mov  x1, #0xc000
            0xF2BFFFE1, // movk x1, #0xffff, lsl #16
            0xF2C001E1, // movk x1, #0xf, lsl #32
            0x39408820, // ldrb w0, [x1, #0x22]
            0xF9401C20, // ldr  x0, [x1, #0x38]
        });
        fixture.memory.write_memory(code_base, code.data(), sizeof(code));
        fixture.backend->reg(sogen::arm64_register::pc, code_base);

        fixture.backend->start(4);
        EXPECT_EQ(fixture.backend->reg(sogen::arm64_register::x0), fixture.info.ncpus);

        fixture.backend->start(1);
        EXPECT_EQ(fixture.backend->reg(sogen::arm64_register::x0), fixture.info.memory_size);
    }

    void expect_store_faults(commpage_fixture& fixture, const uint32_t movz_page_base, const uint32_t store)
    {
        constexpr uint64_t code_base = 0x100000000ULL;
        ASSERT_TRUE(fixture.memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const auto code = std::to_array<uint32_t>({
            movz_page_base, //
            0xF2BFFFE1,     // movk x1, #0xffff, lsl #16
            0xF2C001E1,     // movk x1, #0xf, lsl #32
            store,          //
        });
        fixture.memory.write_memory(code_base, code.data(), sizeof(code));
        fixture.backend->reg(sogen::arm64_register::pc, code_base);
        EXPECT_ANY_THROW(fixture.backend->start(4));
    }

    TEST(MacosCommpage, GuestCannotWriteTheReadWritePage)
    {
        commpage_fixture fixture{};

        expect_store_faults(fixture, 0xD2980001 /* mov x1, #0xc000 */, 0xF9001C3F /* str xzr, [x1, #0x38] */);

        EXPECT_EQ(fixture.read<uint64_t>(commpage_base + 0x038), fixture.info.memory_size);
    }

    TEST(MacosCommpage, GuestCannotWriteTheReadOnlyPage)
    {
        commpage_fixture fixture{};

        expect_store_faults(fixture, 0xD2880001 /* mov x1, #0x4000 */, 0x3900903F /* strb wzr, [x1, #0x24] */);

        EXPECT_EQ(fixture.read<uint8_t>(commpage_ro_base + 0x024), 14u);
    }

    // A capability bit is a promise, not a description: libraries branch on it and execute the
    // instruction without consulting anything else. libcorecrypto reads this word straight off the
    // commpage and, on bit 57, executes MRS DIT -- which this backend does not implement. Its own
    // ID_AA64PFR0_EL1 reports the feature absent, but nothing asks.
    TEST(MacosCommpage, DoesNotPromiseFeaturesTheBackendCannotPerform)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->commpage.is_mapped());

        uint64_t capabilities = 0;
        emu->memory.read_memory(emu->commpage.get_base() + sogen::commpage_offset::CPU_CAPABILITIES64, &capabilities, sizeof(capabilities));

        EXPECT_EQ(capabilities, emu->system_info.cpu_capabilities64) << "the commpage publishes the word verbatim";
        // Written as the literal bit rather than through the constant: testing the constant against
        // itself would pass whichever bit it named.
        constexpr uint64_t dit_bit = uint64_t{1} << 57;
        EXPECT_EQ(sogen::macos_system_info::FEAT_DIT, dit_bit) << "bit 57 was read out of libcorecrypto's own branch";
        EXPECT_EQ(capabilities & dit_bit, 0u);

        // The rest of the word is still the machine's, so this is a subtraction from reality rather
        // than an invention: everything else a real Apple silicon Mac reports is still reported.
        EXPECT_EQ(capabilities, 0x87FFEF97A70E7FC8ULL & ~dit_bit);
    }
}
