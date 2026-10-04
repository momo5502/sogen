#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <macos_memory_manager.hpp>
#include <serialization.hpp>

#include <stdexcept>
#include <vector>

namespace
{
    sogen::utils::buffer_serializer make_crafted_snapshot(const uint64_t start, const size_t length)
    {
        sogen::macos_memory_manager::region_map regions{};
        regions[start] = sogen::macos_memory_region{.length = length, .permissions = sogen::memory_permission::read_write, .backed = true};

        sogen::utils::buffer_serializer serializer{};
        serializer.write<uint64_t>(sogen::MACOS_DEFAULT_MMAP_BASE);
        serializer.write_map(regions);

        const std::vector<uint8_t> data(length);
        serializer.write(data.data(), length);

        return serializer;
    }

    TEST(MacosMemoryManager, AllocatesAtSixteenKiBGranularity)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        ASSERT_TRUE(memory.allocate_memory(0x300000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto info = memory.get_region_info(0x300000000ULL);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->start, 0x300000000ULL);
        EXPECT_EQ(info->length, sogen::MACOS_PAGE_SIZE);
    }

    TEST(MacosMemoryManager, RoundsAnUnalignedRequestUpToAWholeGuestPage)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        ASSERT_TRUE(memory.allocate_memory(0x300000000ULL, 1, sogen::memory_permission::read_write));

        const auto info = memory.get_region_info(0x300000000ULL);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->length, sogen::MACOS_PAGE_SIZE);
    }

    TEST(MacosMemoryManager, RefusesPageZero)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        EXPECT_FALSE(memory.allocate_memory(0, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        EXPECT_FALSE(memory.allocate_memory(0xFFFFC000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        EXPECT_TRUE(memory.allocate_memory(sogen::MACOS_PAGEZERO_END, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
    }

    TEST(MacosMemoryManager, RefusesTheCommpageNestingRegion)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        EXPECT_FALSE(
            memory.allocate_memory(sogen::MACOS_COMMPAGE_NESTING_START, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        EXPECT_FALSE(memory.allocate_memory(sogen::MACOS_COMMPAGE_BASE, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read));
        EXPECT_TRUE(memory.is_reserved_range(sogen::MACOS_COMMPAGE_RO_BASE, sogen::MACOS_PAGE_SIZE));
    }

    TEST(MacosMemoryManager, AllocateReservedRegionIsTheOnlyWayIntoTheNestingRegion)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        ASSERT_TRUE(
            memory.allocate_reserved_region(sogen::MACOS_COMMPAGE_BASE, sogen::MACOS_COMMPAGE_MAP_SIZE, sogen::memory_permission::read));

        const auto info = memory.get_region_info(sogen::MACOS_COMMPAGE_BASE + 0x100);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->start, sogen::MACOS_COMMPAGE_BASE);
        EXPECT_EQ(info->permissions, sogen::memory_permission::read);

        uint8_t byte{};
        EXPECT_TRUE(backend->try_read_memory(sogen::MACOS_COMMPAGE_BASE, &byte, sizeof(byte)));
    }

    TEST(MacosMemoryManager, FloatingAllocationsLandAboveTheSharedCacheSpan)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        EXPECT_EQ(memory.get_mmap_base(), sogen::MACOS_DEFAULT_MMAP_BASE);

        const auto base = memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(base, 0u);
        EXPECT_GE(base, sogen::MACOS_SHARED_CACHE_END);
        EXPECT_LT(base, sogen::MACOS_MAX_MMAP_END_EXCL);
    }

    TEST(MacosMemoryManager, UnhintedAllocationsStayAboveTheSharedCacheWhenTheMmapBaseIsLowered)
    {
        constexpr uint64_t stage_two_mmap_base = 0x110000000ULL;
        static_assert(stage_two_mmap_base < sogen::MACOS_SHARED_CACHE_END);

        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        memory.set_mmap_base(stage_two_mmap_base);

        const auto floored = memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(floored, 0u);
        EXPECT_GE(floored, sogen::MACOS_SHARED_CACHE_END);

        const auto hinted = memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, 0x200000000ULL);
        EXPECT_EQ(hinted, 0x200000000ULL);

        sogen::utils::buffer_serializer serializer{};
        memory.serialize_memory_state(serializer);

        const auto restored_backend = macos_test::make_backend();
        sogen::macos_memory_manager restored{*restored_backend};

        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize_memory_state(deserializer);

        ASSERT_EQ(restored.get_mmap_base(), stage_two_mmap_base);

        const auto after_restore = restored.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_NE(after_restore, 0u);
        EXPECT_GE(after_restore, sogen::MACOS_SHARED_CACHE_END);
    }

    TEST(MacosMemoryManager, ProtectAndReleaseAreObservable)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        constexpr uint64_t base = 0x300000000ULL;
        ASSERT_TRUE(memory.allocate_memory(base, sogen::MACOS_PAGE_SIZE * 2, sogen::memory_permission::read_write));

        ASSERT_TRUE(memory.protect_memory(base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read));
        const auto protected_info = memory.get_region_info(base);
        ASSERT_TRUE(protected_info.has_value());
        EXPECT_EQ(protected_info->permissions, sogen::memory_permission::read);

        ASSERT_TRUE(memory.release_memory(base, sogen::MACOS_PAGE_SIZE));
        EXPECT_FALSE(memory.get_region_info(base).has_value());
        EXPECT_TRUE(memory.get_region_info(base + sogen::MACOS_PAGE_SIZE).has_value());
    }

    TEST(MacosMemoryManager, GuestMemoryRoundTrips)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        constexpr uint64_t base = 0x300000000ULL;
        ASSERT_TRUE(memory.allocate_memory(base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint64_t value = 0xFEEDFACF00001234ULL;
        memory.write_memory(base, &value, sizeof(value));

        uint64_t read_back{};
        memory.read_memory(base, &read_back, sizeof(read_back));
        EXPECT_EQ(read_back, value);
        EXPECT_FALSE(memory.try_read_memory(0x900000000ULL, &read_back, sizeof(read_back)));
    }

    TEST(MacosMemoryManager, UnmapAllMemoryDropsBackedAndReservedRanges)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        ASSERT_TRUE(memory.allocate_memory(0x300000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(memory.reserve_memory(0, sogen::MACOS_PAGEZERO_END));

        memory.unmap_all_memory();

        EXPECT_TRUE(memory.get_mapped_regions().empty());
        EXPECT_TRUE(memory.get_mapped_region_infos().empty());

        uint8_t byte{};
        EXPECT_FALSE(backend->try_read_memory(0x300000000ULL, &byte, sizeof(byte)));
        EXPECT_TRUE(memory.allocate_memory(0x300000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
    }

    TEST(MacosMemoryManager, MemoryStateSurvivesASerializationRoundTrip)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        constexpr uint64_t base = 0x300000000ULL;
        ASSERT_TRUE(memory.allocate_memory(base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(memory.reserve_memory(0, sogen::MACOS_PAGEZERO_END));
        memory.set_mmap_base(0x400000000ULL);

        constexpr uint32_t value = 0x1BADB002;
        memory.write_memory(base, &value, sizeof(value));

        sogen::utils::buffer_serializer serializer{};
        memory.serialize_memory_state(serializer);

        memory.unmap_all_memory();
        memory.set_mmap_base(sogen::MACOS_DEFAULT_MMAP_BASE);

        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        memory.deserialize_memory_state(deserializer);

        EXPECT_EQ(memory.get_mmap_base(), 0x400000000ULL);
        EXPECT_EQ(memory.get_mapped_regions().size(), 2u);

        uint32_t read_back{};
        memory.read_memory(base, &read_back, sizeof(read_back));
        EXPECT_EQ(read_back, value);

        const auto page_zero = memory.get_region_info(0);
        ASSERT_TRUE(page_zero.has_value());
        EXPECT_EQ(page_zero->length, sogen::MACOS_PAGEZERO_END);

        uint8_t byte{};
        EXPECT_FALSE(backend->try_read_memory(0, &byte, sizeof(byte)));
    }

    TEST(MacosMemoryManager, RejectsASnapshotPlacingARegionInTheCommpageNestingRegion)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        auto serializer = make_crafted_snapshot(sogen::MACOS_COMMPAGE_NESTING_START, sogen::MACOS_PAGE_SIZE);
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};

        EXPECT_THROW(memory.deserialize_memory_state(deserializer), std::runtime_error);

        EXPECT_TRUE(memory.get_mapped_regions().empty());

        uint8_t byte{};
        EXPECT_FALSE(backend->try_read_memory(sogen::MACOS_COMMPAGE_NESTING_START, &byte, sizeof(byte)));
    }

    TEST(MacosMemoryManager, RejectsASnapshotPlacingARegionOverPageZero)
    {
        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        auto serializer = make_crafted_snapshot(0, sogen::MACOS_PAGE_SIZE);
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};

        EXPECT_THROW(memory.deserialize_memory_state(deserializer), std::runtime_error);

        EXPECT_TRUE(memory.get_mapped_regions().empty());

        uint8_t byte{};
        EXPECT_FALSE(backend->try_read_memory(0, &byte, sizeof(byte)));
    }

    TEST(MacosMemoryManager, EmulatorOwnedRegionsAreLeftOutOfASnapshot)
    {
        constexpr uint64_t guest_base = 0x300000000ULL;

        const auto backend = macos_test::make_backend();
        sogen::macos_memory_manager memory{*backend};

        ASSERT_TRUE(memory.allocate_memory(guest_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(
            memory.allocate_reserved_region(sogen::MACOS_COMMPAGE_BASE, sogen::MACOS_COMMPAGE_MAP_SIZE, sogen::memory_permission::read));

        sogen::utils::buffer_serializer serializer{};
        memory.serialize_memory_state(serializer);

        const auto restored_backend = macos_test::make_backend();
        sogen::macos_memory_manager restored{*restored_backend};

        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        ASSERT_NO_THROW(restored.deserialize_memory_state(deserializer));

        EXPECT_EQ(restored.get_mapped_regions().size(), 1u);
        EXPECT_TRUE(restored.get_region_info(guest_base).has_value());
        EXPECT_FALSE(restored.get_region_info(sogen::MACOS_COMMPAGE_BASE).has_value());
    }
}
