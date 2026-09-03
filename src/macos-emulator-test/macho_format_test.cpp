#include <gtest/gtest.h>

#include <platform/macho.hpp>

#include "fixture_utils.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <ranges>

namespace
{
    constexpr uint64_t FAT64_ARM64_OFFSET = 0x4000;
    constexpr uint64_t FAT64_ARM64E_OFFSET = 0xc000;
    constexpr uint64_t FAT64_SLICE_SIZE = 16448;

    void put_big_endian(std::vector<std::byte>& data, const uint64_t offset, const uint64_t value, const size_t width)
    {
        for (size_t i = 0; i < width; ++i)
        {
            data[static_cast<size_t>(offset) + i] = static_cast<std::byte>((value >> ((width - 1 - i) * 8)) & 0xff);
        }
    }

    uint64_t fat64_entry_offset(const size_t index)
    {
        return sizeof(sogen::macho::fat_header) + index * sizeof(sogen::macho::fat_arch_64);
    }

    // Fat headers are big-endian on disk, so the fields are laid out byte by byte here rather than through
    // macho::bswap*: building the fixture with the same swap the parser uses would round-trip through any
    // involution and hide a wrong one.
    std::vector<std::byte> synthetic_fat64()
    {
        const auto arm64 = sogen::test::read_fixture("macho_static_arm64");
        const auto arm64e = sogen::test::read_fixture("macho_static_arm64e");

        std::vector<std::byte> data(static_cast<size_t>(FAT64_ARM64E_OFFSET + FAT64_SLICE_SIZE), std::byte{0});

        put_big_endian(data, 0, sogen::macho::FAT_MAGIC_64, 4);
        put_big_endian(data, 4, 2, 4);

        put_big_endian(data, fat64_entry_offset(0), sogen::macho::CPU_TYPE_ARM64, 4);
        put_big_endian(data, fat64_entry_offset(0) + 4, sogen::macho::CPU_SUBTYPE_ARM64_ALL, 4);
        put_big_endian(data, fat64_entry_offset(0) + 8, FAT64_ARM64_OFFSET, 8);
        put_big_endian(data, fat64_entry_offset(0) + 16, arm64.size(), 8);
        put_big_endian(data, fat64_entry_offset(0) + 24, 14, 4);

        put_big_endian(data, fat64_entry_offset(1), sogen::macho::CPU_TYPE_ARM64, 4);
        put_big_endian(data, fat64_entry_offset(1) + 4, sogen::macho::CPU_SUBTYPE_PTRAUTH_ABI | sogen::macho::CPU_SUBTYPE_ARM64E, 4);
        put_big_endian(data, fat64_entry_offset(1) + 8, FAT64_ARM64E_OFFSET, 8);
        put_big_endian(data, fat64_entry_offset(1) + 16, arm64e.size(), 8);
        put_big_endian(data, fat64_entry_offset(1) + 24, 14, 4);

        std::ranges::copy(arm64, data.begin() + static_cast<std::ptrdiff_t>(FAT64_ARM64_OFFSET));
        std::ranges::copy(arm64e, data.begin() + static_cast<std::ptrdiff_t>(FAT64_ARM64E_OFFSET));

        return data;
    }

    std::pair<bool, size_t> walk_fat_arches(const std::vector<std::byte>& data)
    {
        size_t visited = 0;
        const auto ok = sogen::macho::for_each_fat_arch(data, [&](const sogen::macho::fat_slice&) {
            ++visited;
            return true;
        });

        return {ok, visited};
    }

    TEST(MachoFixtures, StaticSlicesHaveTheExpectedSize)
    {
        EXPECT_EQ(sogen::test::read_fixture("macho_static_arm64").size(), 16448u);
        EXPECT_EQ(sogen::test::read_fixture("macho_static_arm64e").size(), 16448u);
        EXPECT_EQ(sogen::test::read_fixture("macho_fat_arm64_arm64e").size(), 65600u);
        EXPECT_EQ(sogen::test::read_fixture("macho_dylink_arm64").size(), 33440u);
    }

    TEST(MachoFixtures, StaticArm64StartsWithMachMagic)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");
        ASSERT_GE(data.size(), 4u);

        EXPECT_EQ(static_cast<uint8_t>(data[0]), 0xcfu);
        EXPECT_EQ(static_cast<uint8_t>(data[1]), 0xfau);
        EXPECT_EQ(static_cast<uint8_t>(data[2]), 0xedu);
        EXPECT_EQ(static_cast<uint8_t>(data[3]), 0xfeu);
    }

    TEST(MachoFixtures, FatBinaryStartsWithBigEndianFatMagic)
    {
        const auto data = sogen::test::read_fixture("macho_fat_arm64_arm64e");
        ASSERT_GE(data.size(), 8u);

        EXPECT_EQ(static_cast<uint8_t>(data[0]), 0xcau);
        EXPECT_EQ(static_cast<uint8_t>(data[1]), 0xfeu);
        EXPECT_EQ(static_cast<uint8_t>(data[2]), 0xbau);
        EXPECT_EQ(static_cast<uint8_t>(data[3]), 0xbeu);
        EXPECT_EQ(static_cast<uint8_t>(data[7]), 0x02u);
    }

    TEST(MachoHeader, RecognisesThinAndFatImages)
    {
        const auto thin = sogen::test::read_fixture("macho_static_arm64");
        const auto fat = sogen::test::read_fixture("macho_fat_arm64_arm64e");

        EXPECT_TRUE(sogen::macho::has_macho_magic(thin));
        EXPECT_FALSE(sogen::macho::is_fat(thin));

        EXPECT_TRUE(sogen::macho::has_macho_magic(fat));
        EXPECT_TRUE(sogen::macho::is_fat(fat));

        const std::vector<std::byte> garbage(64, std::byte{0x41});
        EXPECT_FALSE(sogen::macho::has_macho_magic(garbage));
        EXPECT_FALSE(sogen::macho::is_fat(garbage));
    }

    TEST(MachoHeader, ParsesThinArm64Header)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto* header = sogen::macho::get_header(data, 0);

        ASSERT_NE(header, nullptr);
        EXPECT_EQ(header->magic, sogen::macho::MH_MAGIC_64);
        EXPECT_EQ(header->cputype, sogen::macho::CPU_TYPE_ARM64);
        EXPECT_EQ(header->cpusubtype, sogen::macho::CPU_SUBTYPE_ARM64_ALL);
        EXPECT_EQ(header->filetype, sogen::macho::MH_EXECUTE);
        EXPECT_EQ(header->ncmds, 6u);
        EXPECT_EQ(header->sizeofcmds, 624u);
        EXPECT_EQ(header->flags, sogen::macho::MH_NOUNDEFS);
    }

    TEST(MachoHeader, Arm64eSliceCarriesThePtrauthAbiBit)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64e");
        const auto* header = sogen::macho::get_header(data, 0);

        ASSERT_NE(header, nullptr);
        EXPECT_EQ(header->cpusubtype, 0x80000002u);
        EXPECT_EQ(header->cpusubtype & ~sogen::macho::CPU_SUBTYPE_MASK, sogen::macho::CPU_SUBTYPE_ARM64E);
    }

    TEST(MachoFat, SelectsSlicesByMaskedSubtype)
    {
        const auto data = sogen::test::read_fixture("macho_fat_arm64_arm64e");

        const auto plain = sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64_ALL);
        ASSERT_TRUE(plain.has_value());
        EXPECT_EQ(*plain, 16384u);

        const auto arm64e = sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64E);
        ASSERT_TRUE(arm64e.has_value());
        EXPECT_EQ(*arm64e, 49152u);

        EXPECT_FALSE(sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_X86_64, 3).has_value());
    }

    TEST(MachoFat, RejectsASliceWhoseCommandTableReachesIntoTheNextSlice)
    {
        auto data = sogen::test::read_fixture("macho_fat_arm64_arm64e");

        const auto arm64 = sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64_ALL);
        ASSERT_TRUE(arm64.has_value());

        const auto arm64e = sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64E);
        ASSERT_TRUE(arm64e.has_value());
        ASSERT_LT(*arm64, *arm64e);

        ASSERT_TRUE(sogen::macho::get_load_commands(data, *arm64).has_value());

        constexpr uint32_t forged_sizeofcmds = 40000;
        const auto table_end = *arm64 + sizeof(sogen::macho::mach_header_64) + forged_sizeofcmds;
        ASSERT_GT(table_end, *arm64e);
        ASSERT_LT(table_end, data.size());

        const auto field = static_cast<size_t>(*arm64) + offsetof(sogen::macho::mach_header_64, sizeofcmds);
        for (size_t i = 0; i < sizeof(uint32_t); ++i)
        {
            data[field + i] = static_cast<std::byte>((forged_sizeofcmds >> (i * 8)) & 0xff);
        }

        EXPECT_FALSE(sogen::macho::get_load_commands(data, *arm64).has_value());

        size_t visited = 0;
        const auto ok =
            sogen::macho::for_each_load_command(data, *arm64, [&](const sogen::macho::load_command&, std::span<const std::byte>) {
                ++visited;
                return true;
            });

        EXPECT_FALSE(ok);
        EXPECT_EQ(visited, 0u);
    }

    TEST(MachoFat, SelectedSliceHeaderMatchesTheSliceItNamed)
    {
        const auto data = sogen::test::read_fixture("macho_fat_arm64_arm64e");

        const auto arm64e = sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64E);
        ASSERT_TRUE(arm64e.has_value());

        const auto* header = sogen::macho::get_header(data, *arm64e);
        ASSERT_NE(header, nullptr);
        EXPECT_EQ(header->cpusubtype, 0x80000002u);
    }

    TEST(MachoFat, SwapsBothHalvesOfASixtyFourBitField)
    {
        static_assert(sogen::macho::bswap32(0x11223344u) == 0x44332211u);
        static_assert(sogen::macho::bswap64(0x0011223344556677ULL) == 0x7766554433221100ULL);
        static_assert(sogen::macho::bswap64(0x00000000ffffffffULL) == 0xffffffff00000000ULL);
        static_assert(sogen::macho::bswap64(0x0000000000004000ULL) == 0x0040000000000000ULL);
        SUCCEED();
    }

    TEST(MachoFat, ReadsSixtyFourBitFatEntriesAndSelectsTheArm64eSlice)
    {
        const auto data = synthetic_fat64();

        EXPECT_EQ(static_cast<uint8_t>(data[3]), 0xbfu);
        EXPECT_TRUE(sogen::macho::is_fat(data));
        EXPECT_TRUE(sogen::macho::has_macho_magic(data));

        std::vector<sogen::macho::fat_slice> slices{};
        EXPECT_TRUE(sogen::macho::for_each_fat_arch(data, [&](const sogen::macho::fat_slice& slice) {
            slices.push_back(slice);
            return true;
        }));

        ASSERT_EQ(slices.size(), 2u);
        EXPECT_EQ(slices[0].cputype, sogen::macho::CPU_TYPE_ARM64);
        EXPECT_EQ(slices[0].cpusubtype, sogen::macho::CPU_SUBTYPE_ARM64_ALL);
        EXPECT_EQ(slices[0].offset, FAT64_ARM64_OFFSET);
        EXPECT_EQ(slices[0].size, FAT64_SLICE_SIZE);
        EXPECT_EQ(slices[1].cpusubtype, 0x80000002u);
        EXPECT_EQ(slices[1].offset, FAT64_ARM64E_OFFSET);
        EXPECT_EQ(slices[1].size, FAT64_SLICE_SIZE);

        const auto plain = sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64_ALL);
        ASSERT_TRUE(plain.has_value());
        EXPECT_EQ(*plain, FAT64_ARM64_OFFSET);

        const auto arm64e = sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64E);
        ASSERT_TRUE(arm64e.has_value());
        EXPECT_EQ(*arm64e, FAT64_ARM64E_OFFSET);

        EXPECT_FALSE(sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_X86_64, 3).has_value());

        const auto slice = sogen::macho::get_slice(data, *arm64e);
        ASSERT_TRUE(slice.has_value());
        EXPECT_EQ(slice->size(), FAT64_SLICE_SIZE);

        const auto* header = sogen::macho::get_header(data, *arm64e);
        ASSERT_NE(header, nullptr);
        EXPECT_EQ(header->cputype, sogen::macho::CPU_TYPE_ARM64);
        EXPECT_EQ(header->cpusubtype, 0x80000002u);
        EXPECT_EQ(sogen::macho::get_header(data, *plain)->cpusubtype, sogen::macho::CPU_SUBTYPE_ARM64_ALL);
    }

    TEST(MachoFat, RejectsASixtyFourBitFatEntryWhoseExtentEscapesTheBuffer)
    {
        const auto buffer_size = synthetic_fat64().size();
        const auto offset_field = fat64_entry_offset(0) + 8;
        const auto size_field = fat64_entry_offset(0) + 16;

        const auto expect_rejected = [](const std::vector<std::byte>& data) {
            const auto [ok, visited] = walk_fat_arches(data);
            EXPECT_FALSE(ok);
            EXPECT_EQ(visited, 0u);

            EXPECT_FALSE(sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64_ALL).has_value());
            EXPECT_FALSE(sogen::macho::find_fat_slice(data, sogen::macho::CPU_TYPE_ARM64, sogen::macho::CPU_SUBTYPE_ARM64E).has_value());
            EXPECT_FALSE(sogen::macho::get_slice(data, FAT64_ARM64E_OFFSET).has_value());
        };

        auto beyond_the_end = synthetic_fat64();
        put_big_endian(beyond_the_end, offset_field, buffer_size + 1, 8);
        expect_rejected(beyond_the_end);

        auto oversized = synthetic_fat64();
        put_big_endian(oversized, size_field, buffer_size, 8);
        expect_rejected(oversized);

        // offset + size wraps to exactly 0 here, so the underflow-safe form is the only one that rejects it.
        auto wrapping = synthetic_fat64();
        put_big_endian(wrapping, size_field, std::numeric_limits<uint64_t>::max() - FAT64_ARM64_OFFSET + 1, 8);
        expect_rejected(wrapping);

        auto truncated_entry = synthetic_fat64();
        truncated_entry.resize(static_cast<size_t>(fat64_entry_offset(0)) + sizeof(sogen::macho::fat_arch_64) - 1);
        const auto [ok, visited] = walk_fat_arches(truncated_entry);
        EXPECT_FALSE(ok);
        EXPECT_EQ(visited, 0u);
    }

    TEST(MachoLoadCommands, WalksTheStaticFixtureExactly)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        std::vector<uint32_t> commands{};
        const auto ok =
            sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command& lc, std::span<const std::byte> body) {
                EXPECT_EQ(body.size(), lc.cmdsize);
                commands.push_back(lc.cmd);
                return true;
            });

        ASSERT_TRUE(ok);
        ASSERT_EQ(commands.size(), 6u);
        EXPECT_EQ(commands[0], sogen::macho::LC_SEGMENT_64);
        EXPECT_EQ(commands[1], sogen::macho::LC_SEGMENT_64);
        EXPECT_EQ(commands[2], sogen::macho::LC_SEGMENT_64);
        EXPECT_EQ(commands[3], sogen::macho::LC_SYMTAB);
        EXPECT_EQ(commands[4], sogen::macho::LC_SOURCE_VERSION);
        EXPECT_EQ(commands[5], sogen::macho::LC_UNIXTHREAD);
    }

    TEST(MachoLoadCommands, StopsWhenTheCallbackReturnsFalse)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        size_t visited = 0;
        sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command&, std::span<const std::byte>) {
            ++visited;
            return visited < 2;
        });

        EXPECT_EQ(visited, 2u);
    }

    TEST(MachoLoadCommands, RejectsATruncatedCommandTable)
    {
        auto data = sogen::test::read_fixture("macho_static_arm64");
        data.resize(48);

        size_t visited = 0;
        const auto ok = sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command&, std::span<const std::byte>) {
            ++visited;
            return true;
        });

        EXPECT_FALSE(ok);
        EXPECT_EQ(visited, 0u);
    }

    TEST(MachoLoadCommands, RejectsAZeroSizedCommand)
    {
        auto data = sogen::test::read_fixture("macho_static_arm64");
        data[32 + 4] = std::byte{0};
        data[32 + 5] = std::byte{0};
        data[32 + 6] = std::byte{0};
        data[32 + 7] = std::byte{0};

        const auto ok = sogen::macho::for_each_load_command(data, 0, [](const sogen::macho::load_command&, std::span<const std::byte>) { //
            return true;
        });

        EXPECT_FALSE(ok);
    }
}

namespace
{
    TEST(MachoSegments, ReadsTheStaticFixtureSegmentTable)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        const auto* page_zero = sogen::macho::find_segment(data, 0, "__PAGEZERO");
        ASSERT_NE(page_zero, nullptr);
        EXPECT_EQ(page_zero->vmaddr, 0u);
        EXPECT_EQ(page_zero->vmsize, 0x100000000u);
        EXPECT_EQ(page_zero->filesize, 0u);
        EXPECT_EQ(page_zero->initprot, 0u);
        EXPECT_EQ(page_zero->maxprot, 0u);

        const auto* text = sogen::macho::find_segment(data, 0, "__TEXT");
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->vmaddr, 0x100000000u);
        EXPECT_EQ(text->vmsize, 0x4000u);
        EXPECT_EQ(text->fileoff, 0u);
        EXPECT_EQ(text->filesize, 16384u);
        EXPECT_EQ(text->initprot, sogen::macho::VM_PROT_READ | sogen::macho::VM_PROT_EXECUTE);
        EXPECT_EQ(text->nsects, 1u);

        const auto* linkedit = sogen::macho::find_segment(data, 0, "__LINKEDIT");
        ASSERT_NE(linkedit, nullptr);
        EXPECT_EQ(linkedit->vmaddr, 0x100004000u);
        EXPECT_EQ(linkedit->fileoff, 16384u);
        EXPECT_EQ(linkedit->filesize, 64u);

        EXPECT_EQ(sogen::macho::find_segment(data, 0, "__DATA"), nullptr);
    }

    TEST(MachoSegments, MachHeaderVmaddrSkipsPageZero)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        const auto base = sogen::macho::mach_header_vmaddr(data, 0);
        ASSERT_TRUE(base.has_value());
        EXPECT_EQ(*base, 0x100000000u);
    }

    TEST(MachoEntryPoint, ReadsUnixthreadProgramCounter)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        std::optional<uint64_t> pc{};
        sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command& lc, std::span<const std::byte> body) {
            if (lc.cmd != sogen::macho::LC_UNIXTHREAD)
            {
                return true;
            }

            const auto thread = sogen::macho::read_at<sogen::macho::thread_command>(body, 0);
            EXPECT_TRUE(thread.has_value());
            EXPECT_EQ(thread->flavor, sogen::macho::ARM_THREAD_STATE64);
            EXPECT_EQ(thread->count, sogen::macho::ARM_THREAD_STATE64_COUNT);

            const auto state = sogen::macho::read_at<sogen::macho::arm_thread_state64_t>(body, sizeof(sogen::macho::thread_command));
            EXPECT_TRUE(state.has_value());
            pc = state->pc;
            return false;
        });

        ASSERT_TRUE(pc.has_value());
        EXPECT_EQ(*pc, 0x1000002d0u);
    }

    TEST(MachoEntryPoint, ReadsMainCommandAndDylinkerPath)
    {
        const auto data = sogen::test::read_fixture("macho_dylink_arm64");

        std::optional<uint64_t> entryoff{};
        std::optional<uint64_t> stacksize{};
        std::string dylinker{};

        sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command& lc, std::span<const std::byte> body) {
            if (lc.cmd == sogen::macho::LC_MAIN)
            {
                const auto main_cmd = sogen::macho::read_at<sogen::macho::entry_point_command>(body, 0);
                EXPECT_TRUE(main_cmd.has_value());
                entryoff = main_cmd->entryoff;
                stacksize = main_cmd->stacksize;
            }
            else if (lc.cmd == sogen::macho::LC_LOAD_DYLINKER)
            {
                const auto cmd = sogen::macho::read_at<sogen::macho::dylinker_command>(body, 0);
                EXPECT_TRUE(cmd.has_value());
                dylinker = sogen::macho::read_lc_str(body, cmd->name);
            }

            return true;
        });

        ASSERT_TRUE(entryoff.has_value());
        EXPECT_EQ(*entryoff, 1096u);
        ASSERT_TRUE(stacksize.has_value());
        EXPECT_EQ(*stacksize, 0u);
        EXPECT_EQ(dylinker, "/usr/lib/dyld");
    }

    TEST(MachoEntryPoint, StaticFixtureHasNoMainCommand)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        bool found = false;
        sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command& lc, std::span<const std::byte>) {
            found = found || lc.cmd == sogen::macho::LC_MAIN || lc.cmd == sogen::macho::LC_LOAD_DYLINKER;
            return true;
        });

        EXPECT_FALSE(found);
    }

    TEST(MachoLcStr, ClampsToTheCommandBody)
    {
        const auto data = sogen::test::read_fixture("macho_dylink_arm64");

        std::string_view out{"unset"};
        sogen::macho::for_each_load_command(data, 0, [&](const sogen::macho::load_command& lc, std::span<const std::byte> body) {
            if (lc.cmd != sogen::macho::LC_LOAD_DYLINKER)
            {
                return true;
            }

            out = sogen::macho::read_lc_str(body, lc.cmdsize + 8);
            return false;
        });

        EXPECT_TRUE(out.empty());
    }
}
