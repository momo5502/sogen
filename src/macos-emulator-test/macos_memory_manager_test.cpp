#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

#include <macos_memory_manager.hpp>
#include <unicorn_arm64_emulator.hpp>
#include <utils/io.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
    struct managed_guest
    {
        std::unique_ptr<sogen::arm64_mappable_emulator> emulator{sogen::unicorn::create_arm64_emulator()};
        sogen::macos_memory_manager memory{*emulator};
    };

    void* aligned_host_page()
    {
        alignas(sogen::MACOS_PAGE_SIZE) static std::array<std::byte, sogen::MACOS_PAGE_SIZE> page{};
        return page.data();
    }

    TEST(MacosMemoryManager, AllocatesAndReadsBackGuestMemory)
    {
        managed_guest guest{};

        ASSERT_TRUE(guest.memory.allocate_memory(0x100000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint32_t value = 0xCAFEBABE;
        guest.memory.write_memory(0x100000000ULL, &value, sizeof(value));

        uint32_t read_back{};
        guest.memory.read_memory(0x100000000ULL, &read_back, sizeof(read_back));
        EXPECT_EQ(read_back, value);
    }

    TEST(MacosMemoryManager, RejectsOverlappingAllocations)
    {
        managed_guest guest{};

        ASSERT_TRUE(guest.memory.allocate_memory(0x100000000ULL, 2 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        EXPECT_TRUE(guest.memory.overlaps_mapped_region(0x100004000ULL, sogen::MACOS_PAGE_SIZE));
        EXPECT_FALSE(guest.memory.allocate_memory(0x100004000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
    }

    TEST(MacosMemoryManager, ReservedRangesAreClaimedButNeverBacked)
    {
        managed_guest guest{};

        ASSERT_TRUE(guest.memory.reserve_memory(0, 0x100000000ULL));

        EXPECT_TRUE(guest.memory.overlaps_mapped_region(0, sogen::MACOS_PAGE_SIZE));
        EXPECT_FALSE(guest.memory.allocate_memory(0, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        uint8_t byte{};
        EXPECT_FALSE(guest.emulator->try_read_memory(0, &byte, sizeof(byte)));
        EXPECT_FALSE(guest.emulator->try_read_memory(0xFFFFFFF0ULL, &byte, sizeof(byte)));

        const auto& regions = guest.memory.get_mapped_regions();
        const auto entry = regions.find(0);
        ASSERT_NE(entry, regions.end());
        EXPECT_FALSE(entry->second.backed);
    }

    TEST(MacosMemoryManager, ProtectDowngradesAnAllocatedRange)
    {
        managed_guest guest{};

        ASSERT_TRUE(guest.memory.allocate_memory(0x100000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint32_t value = 0x11223344;
        guest.memory.write_memory(0x100000000ULL, &value, sizeof(value));

        ASSERT_TRUE(guest.memory.protect_memory(0x100000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_exec));

        const auto& regions = guest.memory.get_mapped_regions();
        const auto entry = regions.find(0x100000000ULL);
        ASSERT_NE(entry, regions.end());
        EXPECT_EQ(entry->second.permissions, sogen::memory_permission::read_exec);

        uint32_t read_back{};
        guest.memory.read_memory(0x100000000ULL, &read_back, sizeof(read_back));
        EXPECT_EQ(read_back, value);
    }

    TEST(MacosMemoryManager, ReleaseUnmapsTheRange)
    {
        managed_guest guest{};

        ASSERT_TRUE(guest.memory.allocate_memory(0x100000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(guest.memory.release_memory(0x100000000ULL, sogen::MACOS_PAGE_SIZE));

        uint8_t byte{};
        EXPECT_FALSE(guest.emulator->try_read_memory(0x100000000ULL, &byte, sizeof(byte)));
        EXPECT_TRUE(guest.memory.get_mapped_regions().empty());
    }

    TEST(MacosMemoryManager, FindsAFreeBaseAboveExistingAllocations)
    {
        managed_guest guest{};

        ASSERT_TRUE(
            guest.memory.allocate_memory(sogen::MACOS_DEFAULT_MMAP_BASE, 4 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto base = guest.memory.find_free_allocation_base(sogen::MACOS_PAGE_SIZE);
        ASSERT_NE(base, 0u);
        EXPECT_EQ(base % sogen::MACOS_PAGE_SIZE, 0u);
        EXPECT_FALSE(guest.memory.overlaps_mapped_region(base, sogen::MACOS_PAGE_SIZE));
    }

    TEST(MacosMemoryManager, RejectsRangesOutsideTheAddressSpace)
    {
        managed_guest guest{};

        constexpr auto huge = std::numeric_limits<size_t>::max();

        EXPECT_FALSE(guest.memory.reserve_memory(0x7FFFFFFFFFFFULL, huge));
        EXPECT_FALSE(guest.memory.allocate_memory(0x7FFFFFFFFFFFULL, huge, sogen::memory_permission::read_write));
        EXPECT_FALSE(guest.memory.reserve_memory(sogen::MACOS_MAX_MMAP_END_EXCL, sogen::MACOS_PAGE_SIZE));
        EXPECT_FALSE(guest.memory.allocate_memory(0xFFFFFFFFFFFFC000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        EXPECT_FALSE(guest.memory.reserve_memory(0x100000000ULL, 0));
        EXPECT_FALSE(guest.memory.release_memory(0x100000000ULL, 0));

        EXPECT_TRUE(guest.memory.get_mapped_regions().empty());
    }

    // Replaces MacosMemoryManager.ProtectRequiresAnExactRegionMatch: sys_mprotect takes arbitrary
    // sub-ranges, so an exact [base, length) match is no longer the contract. The rejections that
    // survive splitting - an unmapped range, a range spanning a hole, an unbacked reservation - are
    // kept here.
    TEST(MacosMemoryManager, ProtectSplitsARegionAroundASubRange)
    {
        managed_guest guest{};

        ASSERT_TRUE(guest.memory.allocate_memory(0x100000000ULL, 3 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(guest.memory.allocate_memory(0x100010000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(guest.memory.reserve_memory(0x300000000ULL, sogen::MACOS_PAGE_SIZE));

        EXPECT_FALSE(guest.memory.protect_memory(0x200000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_exec));
        EXPECT_FALSE(guest.memory.protect_memory(0x100008000ULL, 3 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_exec));
        EXPECT_FALSE(guest.memory.protect_memory(0x300000000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_exec));

        const auto& regions = guest.memory.get_mapped_regions();
        ASSERT_EQ(regions.size(), 3u);
        EXPECT_EQ(regions.at(0x100000000ULL).length, 3 * sogen::MACOS_PAGE_SIZE);
        EXPECT_EQ(regions.at(0x100000000ULL).permissions, sogen::memory_permission::read_write);

        ASSERT_TRUE(guest.memory.protect_memory(0x100004000ULL, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_exec));

        ASSERT_EQ(regions.size(), 5u);
        EXPECT_EQ(regions.at(0x100000000ULL).length, sogen::MACOS_PAGE_SIZE);
        EXPECT_EQ(regions.at(0x100000000ULL).permissions, sogen::memory_permission::read_write);
        EXPECT_EQ(regions.at(0x100004000ULL).length, sogen::MACOS_PAGE_SIZE);
        EXPECT_EQ(regions.at(0x100004000ULL).permissions, sogen::memory_permission::read_exec);
        EXPECT_EQ(regions.at(0x100008000ULL).length, sogen::MACOS_PAGE_SIZE);
        EXPECT_EQ(regions.at(0x100008000ULL).permissions, sogen::memory_permission::read_write);

        EXPECT_TRUE(guest.memory.protect_memory(0x100000000ULL, 3 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read));
        EXPECT_EQ(regions.at(0x100000000ULL).permissions, sogen::memory_permission::read);
        EXPECT_EQ(regions.at(0x100004000ULL).permissions, sogen::memory_permission::read);
        EXPECT_EQ(regions.at(0x100008000ULL).permissions, sogen::memory_permission::read);
    }

    // Replaces MacosMemoryManager.ReleaseRequiresAnExactRegionMatch, for the same reason: sys_munmap
    // takes arbitrary sub-ranges and must leave the remainder mapped.
    TEST(MacosMemoryManager, ReleaseSplitsARegionAroundASubRange)
    {
        managed_guest guest{};

        ASSERT_TRUE(guest.memory.allocate_memory(0x100000000ULL, 3 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        EXPECT_FALSE(guest.memory.release_memory(0x200000000ULL, sogen::MACOS_PAGE_SIZE));
        EXPECT_EQ(guest.memory.get_mapped_regions().size(), 1u);

        ASSERT_TRUE(guest.memory.release_memory(0x100004000ULL, sogen::MACOS_PAGE_SIZE));

        const auto& regions = guest.memory.get_mapped_regions();
        ASSERT_EQ(regions.size(), 2u);
        EXPECT_EQ(regions.at(0x100000000ULL).length, sogen::MACOS_PAGE_SIZE);
        EXPECT_EQ(regions.at(0x100008000ULL).length, sogen::MACOS_PAGE_SIZE);

        uint8_t byte{};
        EXPECT_TRUE(guest.emulator->try_read_memory(0x100000000ULL, &byte, sizeof(byte)));
        EXPECT_FALSE(guest.emulator->try_read_memory(0x100004000ULL, &byte, sizeof(byte)));
        EXPECT_TRUE(guest.emulator->try_read_memory(0x100008000ULL, &byte, sizeof(byte)));

        EXPECT_TRUE(guest.memory.release_memory(0x100000000ULL, 3 * sogen::MACOS_PAGE_SIZE));
        EXPECT_TRUE(guest.memory.get_mapped_regions().empty());
    }

    TEST(MacosMemoryManager, FindFreeAllocationBaseRejectsDegenerateSizes)
    {
        managed_guest guest{};

        EXPECT_EQ(guest.memory.find_free_allocation_base(0), 0u);
        EXPECT_EQ(guest.memory.find_free_allocation_base(std::numeric_limits<size_t>::max()), 0u);
        EXPECT_EQ(guest.memory.find_free_allocation_base(static_cast<size_t>(sogen::MACOS_MAX_MMAP_END_EXCL)), 0u);
        EXPECT_EQ(guest.memory.allocate_memory(0, sogen::memory_permission::read_write), 0u);
        EXPECT_EQ(guest.memory.allocate_memory(std::numeric_limits<size_t>::max(), sogen::memory_permission::read_write), 0u);

        EXPECT_TRUE(guest.memory.get_mapped_regions().empty());
    }

    TEST(MacosMemoryManager, AllocatesAtAnAutomaticallyChosenBase)
    {
        managed_guest guest{};

        const auto first = guest.memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_EQ(first, sogen::MACOS_DEFAULT_MMAP_BASE);

        const auto second = guest.memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        ASSERT_EQ(second, sogen::MACOS_DEFAULT_MMAP_BASE + sogen::MACOS_PAGE_SIZE);

        const auto hinted = guest.memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, 0x200000000ULL);
        EXPECT_EQ(hinted, 0x200000000ULL);

        constexpr uint32_t value = 0x5A5A5A5A;
        guest.memory.write_memory(second, &value, sizeof(value));

        uint32_t read_back{};
        guest.memory.read_memory(second, &read_back, sizeof(read_back));
        EXPECT_EQ(read_back, value);

        EXPECT_EQ(guest.memory.get_mapped_regions().size(), 3u);
    }

    TEST(MacosMemoryManager, HostFileMappingRejectsNullAndMisalignedRequests)
    {
        managed_guest guest{};

        auto* page = aligned_host_page();

        EXPECT_FALSE(guest.memory.map_host_file_memory(0x180000000ULL, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), nullptr,
                                                       sogen::memory_permission::read_write));
        EXPECT_FALSE(guest.memory.map_host_file_memory(0x180001000ULL, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), page,
                                                       sogen::memory_permission::read_write));
        EXPECT_FALSE(guest.memory.map_host_file_memory(0x180000000ULL, 0x1000, page, sogen::memory_permission::read_write));
        EXPECT_FALSE(guest.memory.map_host_file_memory(0x180000000ULL, 0, page, sogen::memory_permission::read_write));

        EXPECT_TRUE(guest.memory.get_mapped_regions().empty());

        ASSERT_TRUE(guest.memory.map_host_file_memory(0x180000000ULL, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), page,
                                                      sogen::memory_permission::read_write));
        EXPECT_FALSE(guest.memory.map_host_file_memory(0x180000000ULL, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), page,
                                                       sogen::memory_permission::read_write));
        EXPECT_EQ(guest.memory.get_mapped_regions().size(), 1u);
    }

#ifndef _WIN32
    constexpr uint32_t marker = 0xDEADBEEF;

    uint32_t read_host_marker(const void* host)
    {
        uint32_t value{};
        std::memcpy(&value, host, sizeof(value));
        return value;
    }

    TEST(MacosMemoryManager, HostFileMappingIsCopyOnWrite)
    {
        const auto path = std::filesystem::temp_directory_path() / "sogen_macho_cow_fixture.bin";

        std::vector<std::byte> original(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), std::byte{0xAB});
        ASSERT_TRUE(sogen::utils::io::write_file(path, original));

        const auto fd = ::open(path.string().c_str(), O_RDONLY);
        ASSERT_GE(fd, 0);

        void* host = ::mmap(nullptr, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        ASSERT_NE(host, MAP_FAILED);
        ::close(fd);

        {
            managed_guest guest{};

            ASSERT_TRUE(guest.memory.map_host_file_memory(0x180000000ULL, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), host,
                                                          sogen::memory_permission::read_write));

            uint8_t first{};
            guest.memory.read_memory(0x180000000ULL, &first, sizeof(first));
            EXPECT_EQ(first, 0xABu);

            guest.memory.write_memory(0x180000000ULL, &marker, sizeof(marker));

            uint32_t read_back{};
            guest.memory.read_memory(0x180000000ULL, &read_back, sizeof(read_back));
            EXPECT_EQ(read_back, marker);

            EXPECT_EQ(read_host_marker(host), marker) << "uc_mem_map_ptr copied the page instead of aliasing it";

            ASSERT_TRUE(guest.memory.release_memory(0x180000000ULL, static_cast<size_t>(sogen::MACOS_PAGE_SIZE)));

            uint8_t byte{};
            EXPECT_FALSE(guest.emulator->try_read_memory(0x180000000ULL, &byte, sizeof(byte)));
            EXPECT_EQ(read_host_marker(host), marker) << "releasing the guest range freed the caller's host mapping";
        }

        EXPECT_EQ(read_host_marker(host), marker) << "destroying the emulator freed the caller's host mapping";

        ::munmap(host, static_cast<size_t>(sogen::MACOS_PAGE_SIZE));

        const auto on_disk = sogen::utils::io::read_file(path);
        ASSERT_EQ(on_disk.size(), original.size());
        EXPECT_EQ(on_disk, original) << "guest write reached the backing file - MAP_PRIVATE COW is not holding";

        std::filesystem::remove(path);
    }
#endif
}
