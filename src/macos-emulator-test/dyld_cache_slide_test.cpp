#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <module/dyld_cache_slide.hpp>

#include "macos_test_utils.hpp"

namespace
{
    constexpr uint64_t target_base = 0x280000000ULL;
    constexpr uint64_t slide_base = 0x290000000ULL;
    constexpr uint32_t page_count = 4;
    constexpr uint64_t value_add = 0x180000000ULL;

    uint64_t unauthenticated_entry(const uint64_t runtime_offset, const uint32_t next, const uint8_t high8)
    {
        return (runtime_offset & ((uint64_t{1} << 34) - 1)) | (static_cast<uint64_t>(high8) << 34) |
               (static_cast<uint64_t>(next & 0x7FF) << 52);
    }

    uint64_t authenticated_entry(const uint64_t runtime_offset, const uint32_t next, const uint16_t diversity, const bool addr_div,
                                 const bool key_is_data)
    {
        return (runtime_offset & ((uint64_t{1} << 34) - 1)) | (static_cast<uint64_t>(diversity) << 34) |
               (static_cast<uint64_t>(addr_div ? 1 : 0) << 50) | (static_cast<uint64_t>(key_is_data ? 1 : 0) << 51) |
               (static_cast<uint64_t>(next & 0x7FF) << 52) | (uint64_t{1} << 63);
    }

    struct slide_fixture
    {
        std::unique_ptr<sogen::macos_emulator> emu;
        uint64_t page_size{};
        uint64_t region_size{};
    };

    // A region of four pages, three of which carry a two-entry chain and one of which is marked as having
    // nothing to rebase, plus the version-5 slide blob that describes them.
    slide_fixture build_fixture()
    {
        slide_fixture fixture{};
        fixture.emu = macos_test::make_emulator();
        fixture.page_size = sogen::MACOS_PAGE_SIZE;
        fixture.region_size = fixture.page_size * page_count;

        auto& memory = fixture.emu->memory;
        if (!memory.allocate_memory(target_base, static_cast<size_t>(fixture.region_size), sogen::memory_permission::read_write) ||
            !memory.allocate_memory(slide_base, static_cast<size_t>(fixture.page_size), sogen::memory_permission::read_write))
        {
            fixture.emu.reset();
            return fixture;
        }

        for (uint32_t page = 0; page < page_count; ++page)
        {
            if (page == 2)
            {
                continue;
            }

            const auto page_base = target_base + page * fixture.page_size;

            const auto first = unauthenticated_entry(0x1000 + page * 0x100, 2, static_cast<uint8_t>(page));
            const auto second = unauthenticated_entry(0x2000 + page * 0x100, 0, 0);

            memory.write_memory(page_base, &first, sizeof(first));
            memory.write_memory(page_base + 16, &second, sizeof(second));
        }

        const uint32_t version = 5;
        const auto page_size_field = static_cast<uint32_t>(fixture.page_size);
        const uint32_t starts_count = page_count;

        memory.write_memory(slide_base, &version, sizeof(version));
        memory.write_memory(slide_base + 4, &page_size_field, sizeof(page_size_field));
        memory.write_memory(slide_base + 8, &starts_count, sizeof(starts_count));
        memory.write_memory(slide_base + 16, &value_add, sizeof(value_add));

        for (uint32_t page = 0; page < page_count; ++page)
        {
            const uint16_t start = (page == 2) ? 0xFFFF : 0;
            memory.write_memory(slide_base + 24 + page * sizeof(uint16_t), &start, sizeof(start));
        }

        return fixture;
    }

    std::vector<uint8_t> snapshot(sogen::macos_emulator& emu, const uint64_t size)
    {
        std::vector<uint8_t> data(static_cast<size_t>(size));
        emu.memory.read_memory(target_base, data.data(), data.size());
        return data;
    }

    // A process whose main executable is plain arm64 runs with the pointer-authentication keys off, so
    // the cache's authenticated entries have to be rebased to the bare address. Signing them anyway
    // would leave a signature that nothing strips -- with the keys off autda is a no-op -- and the first
    // dereference would fault on the signature bits rather than reach the pointer.
    TEST(DyldCacheSlide, LeavesAuthenticatedEntriesUnsignedWhenTheKeysAreOff)
    {
        constexpr uint64_t runtime_offset = 0x1000;

        auto with_keys = [](const bool enabled) {
            auto fixture = build_fixture();
            EXPECT_NE(fixture.emu, nullptr);

            const auto entry = authenticated_entry(runtime_offset, 0, 0x6AE1, true, true);
            fixture.emu->memory.write_memory(target_base, &entry, sizeof(entry));
            fixture.emu->pointer_authentication = enabled;

            uint64_t applied = 0;
            EXPECT_TRUE(sogen::apply_dyld_cache_slide_info(*fixture.emu, target_base, static_cast<size_t>(fixture.region_size), slide_base,
                                                           applied));

            uint64_t value = 0;
            fixture.emu->memory.read_memory(target_base, &value, sizeof(value));
            return value;
        };

        const auto unsigned_value = with_keys(false);
        const auto signed_value = with_keys(true);

        EXPECT_EQ(unsigned_value, value_add + runtime_offset) << "with the keys off the entry rebases to the bare address";
        EXPECT_NE(signed_value, unsigned_value) << "with the keys on the same entry carries a signature";
        EXPECT_EQ(signed_value & 0x0000FFFFFFFFFFFFULL, unsigned_value) << "and the signature sits above the address it signs";
    }

    TEST(DyldCacheSlide, RebasesTheChainsItIsGiven)
    {
        auto fixture = build_fixture();
        ASSERT_NE(fixture.emu, nullptr);

        uint64_t applied = 0;
        ASSERT_TRUE(
            sogen::apply_dyld_cache_slide_info(*fixture.emu, target_base, static_cast<size_t>(fixture.region_size), slide_base, applied));

        EXPECT_EQ(applied, 6ULL) << "three pages of two entries each";

        uint64_t first = 0;
        fixture.emu->memory.read_memory(target_base, &first, sizeof(first));
        EXPECT_EQ(first, value_add + 0x1000);

        uint64_t second = 0;
        fixture.emu->memory.read_memory(target_base + 16, &second, sizeof(second));
        EXPECT_EQ(second, value_add + 0x2000);

        // The page marked 0xFFFF must be left exactly as it was.
        uint64_t untouched = 0;
        fixture.emu->memory.read_memory(target_base + 2 * fixture.page_size, &untouched, sizeof(untouched));
        EXPECT_EQ(untouched, 0ULL);
    }

    // The property the pager rests on. A chain never leaves its own page -- its start comes from that
    // page's page_starts entry and every step is an offset within the page -- so rebasing a subset must
    // give bit-for-bit what rebasing everything would have given for those pages. If that ever stopped
    // holding, the pager would produce a cache that differs from the eagerly mapped one in ways nothing
    // else would notice until the guest jumped through a wrong pointer.
    TEST(DyldCacheSlide, ApplyingPageByPageMatchesApplyingEverything)
    {
        auto whole = build_fixture();
        ASSERT_NE(whole.emu, nullptr);

        uint64_t applied_whole = 0;
        ASSERT_TRUE(
            sogen::apply_dyld_cache_slide_info(*whole.emu, target_base, static_cast<size_t>(whole.region_size), slide_base, applied_whole));

        const auto expected = snapshot(*whole.emu, whole.region_size);

        auto piecewise = build_fixture();
        ASSERT_NE(piecewise.emu, nullptr);

        uint64_t applied_pieces = 0;
        for (uint32_t page = 0; page < page_count; ++page)
        {
            const auto begin = target_base + page * piecewise.page_size;
            ASSERT_TRUE(sogen::apply_dyld_cache_slide_info(*piecewise.emu, target_base, static_cast<size_t>(piecewise.region_size),
                                                           slide_base, applied_pieces, begin, begin + piecewise.page_size))
                << "page " << page;
        }

        EXPECT_EQ(applied_pieces, applied_whole);
        EXPECT_EQ(snapshot(*piecewise.emu, piecewise.region_size), expected);
    }

    TEST(DyldCacheSlide, ARestrictedRangeTouchesNothingOutsideIt)
    {
        auto fixture = build_fixture();
        ASSERT_NE(fixture.emu, nullptr);

        uint64_t applied = 0;
        ASSERT_TRUE(sogen::apply_dyld_cache_slide_info(*fixture.emu, target_base, static_cast<size_t>(fixture.region_size), slide_base,
                                                       applied, target_base, target_base + fixture.page_size));

        EXPECT_EQ(applied, 2ULL) << "only the first page's chain";

        uint64_t rebased = 0;
        fixture.emu->memory.read_memory(target_base, &rebased, sizeof(rebased));
        EXPECT_EQ(rebased, value_add + 0x1000);

        // The second page still holds its raw chain entry, not a rebased pointer.
        uint64_t raw = 0;
        fixture.emu->memory.read_memory(target_base + fixture.page_size, &raw, sizeof(raw));
        EXPECT_EQ(raw, unauthenticated_entry(0x1100, 2, 1));
    }

    // Pins the boundary rule rather than leaving it to whichever comparison was written first. A page
    // only half-covered by the restriction is skipped entirely: rebasing it would write past the end of
    // the chunk asking for it, into memory that chunk has not mapped.
    TEST(DyldCacheSlide, APageStraddlingTheBoundaryIsLeftAlone)
    {
        auto fixture = build_fixture();
        ASSERT_NE(fixture.emu, nullptr);

        const auto half = fixture.page_size / 2;

        uint64_t applied = 0;
        ASSERT_TRUE(sogen::apply_dyld_cache_slide_info(*fixture.emu, target_base, static_cast<size_t>(fixture.region_size), slide_base,
                                                       applied, target_base + half, target_base + fixture.page_size + half));

        EXPECT_EQ(applied, 0ULL) << "neither page lies wholly inside the range";

        uint64_t first = 0;
        fixture.emu->memory.read_memory(target_base, &first, sizeof(first));
        EXPECT_EQ(first, unauthenticated_entry(0x1000, 2, 0)) << "the first page was rebased despite hanging out of the range";

        uint64_t second = 0;
        fixture.emu->memory.read_memory(target_base + fixture.page_size, &second, sizeof(second));
        EXPECT_EQ(second, unauthenticated_entry(0x1100, 2, 1)) << "the second page was rebased despite hanging out of the range";
    }
}
