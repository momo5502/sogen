#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <span>
#include <vector>

#include <host_range_reader.hpp>
#include <gui/macos_native_dispatch.hpp>
#include <module/dyld_cache_pager.hpp>

#include "macos_test_utils.hpp"

namespace
{
    constexpr uint64_t CACHE_BASE = 0x180000000ULL;
    constexpr uint64_t REGION_SIZE = 8ULL * sogen::MACOS_CACHE_CHUNK_SIZE;

    class vector_range_reader : public sogen::host_range_reader
    {
      public:
        explicit vector_range_reader(std::vector<std::byte> data)
            : data_(std::move(data))
        {
        }

        uint64_t file_size(const std::string&) override
        {
            return this->data_.size();
        }

        size_t read(const std::string&, const uint64_t offset, const std::span<std::byte> destination) override
        {
            if (destination.empty() || offset >= this->data_.size())
            {
                return 0;
            }

            const auto available = std::min<uint64_t>(destination.size(), this->data_.size() - offset);
            std::memcpy(destination.data(), this->data_.data() + offset, static_cast<size_t>(available));

            ++this->reads;
            this->bytes += available;

            return static_cast<size_t>(available);
        }

        uint64_t reads{};
        uint64_t bytes{};

      private:
        std::vector<std::byte> data_;
    };

    std::vector<std::byte> make_backing_data(const uint64_t size)
    {
        std::vector<std::byte> data(static_cast<size_t>(size));
        for (size_t i = 0; i < data.size(); ++i)
        {
            data[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
        }

        return data;
    }

    std::vector<sogen::dyld_cache_backing_range> make_ranges(const sogen::memory_permission permissions)
    {
        return {sogen::dyld_cache_backing_range{
            .address = CACHE_BASE,
            .size = REGION_SIZE,
            .path = "/fake/dyld_shared_cache_arm64e",
            .file_offset = 0,
            .permissions = permissions,
        }};
    }

    TEST(DyldCachePager, CoversOnlyTheCacheSpan)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)};

        EXPECT_EQ(pager.span_start(), CACHE_BASE);
        EXPECT_EQ(pager.span_end(), CACHE_BASE + REGION_SIZE);
        EXPECT_TRUE(pager.covers(CACHE_BASE));
        EXPECT_TRUE(pager.covers(CACHE_BASE + REGION_SIZE - 1));
        EXPECT_FALSE(pager.covers(CACHE_BASE + REGION_SIZE));
        EXPECT_FALSE(pager.covers(CACHE_BASE - 1));
        EXPECT_FALSE(pager.covers(0));
    }

    TEST(DyldCachePager, PagesInOneChunkWithTheRightBytes)
    {
        const auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(REGION_SIZE);
        vector_range_reader reader{backing};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)};

        const uint64_t probe = CACHE_BASE + sogen::MACOS_CACHE_CHUNK_SIZE + 0x1234;
        ASSERT_TRUE(pager.page_in(probe));

        EXPECT_EQ(pager.paged_in_chunks(), 1ULL);
        EXPECT_EQ(pager.resident_chunks(), 1ULL);
        EXPECT_EQ(pager.resident_bytes(), sogen::MACOS_CACHE_CHUNK_SIZE);

        std::array<std::byte, 8> guest{};
        ASSERT_TRUE(emu->memory.try_read_memory(probe, guest.data(), guest.size()));

        const auto expected = std::span{backing}.subspan(static_cast<size_t>(probe - CACHE_BASE), guest.size());
        EXPECT_EQ(std::memcmp(guest.data(), expected.data(), guest.size()), 0);
    }

    TEST(DyldCachePager, SecondTouchOfTheSameChunkDoesNotReread)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)};

        ASSERT_TRUE(pager.page_in(CACHE_BASE));
        const auto reads_after_first = reader.reads;

        ASSERT_TRUE(pager.page_in(CACHE_BASE + 0x400));

        EXPECT_EQ(reader.reads, reads_after_first);
        EXPECT_EQ(pager.paged_in_chunks(), 1ULL);
    }

    TEST(DyldCachePager, RejectsAddressesOutsideEveryRange)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)};

        EXPECT_FALSE(pager.page_in(CACHE_BASE + REGION_SIZE));
        EXPECT_FALSE(pager.page_in(0));
        EXPECT_EQ(pager.paged_in_chunks(), 0ULL);
    }

    TEST(DyldCachePager, EvictsReadOnlyChunksToStayUnderBudget)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec),
                                      2 * sogen::MACOS_CACHE_CHUNK_SIZE};

        ASSERT_TRUE(pager.page_in(CACHE_BASE + 0 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_TRUE(pager.page_in(CACHE_BASE + 1 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_TRUE(pager.page_in(CACHE_BASE + 2 * sogen::MACOS_CACHE_CHUNK_SIZE));

        EXPECT_EQ(pager.paged_in_chunks(), 3ULL);
        EXPECT_EQ(pager.evicted_chunks(), 1ULL);
        EXPECT_EQ(pager.resident_chunks(), 2ULL);
        EXPECT_LE(pager.resident_bytes(), 2 * sogen::MACOS_CACHE_CHUNK_SIZE);

        std::array<std::byte, 4> probe{};
        EXPECT_FALSE(emu->memory.try_read_memory(CACHE_BASE, probe.data(), probe.size()));
        EXPECT_TRUE(emu->memory.try_read_memory(CACHE_BASE + 2 * sogen::MACOS_CACHE_CHUNK_SIZE, probe.data(), probe.size()));
    }

    // Evicting a writable chunk would throw away whatever the guest wrote into cache DATA, and nothing
    // would ever read it back -- the backing file holds the original bytes, not the guest's.
    TEST(DyldCachePager, NeverEvictsWritableChunks)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_write),
                                      2 * sogen::MACOS_CACHE_CHUNK_SIZE};

        ASSERT_TRUE(pager.page_in(CACHE_BASE + 0 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_TRUE(pager.page_in(CACHE_BASE + 1 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_TRUE(pager.page_in(CACHE_BASE + 2 * sogen::MACOS_CACHE_CHUNK_SIZE));

        EXPECT_EQ(pager.evicted_chunks(), 0ULL);
        EXPECT_EQ(pager.resident_chunks(), 3ULL);

        std::array<std::byte, 4> probe{};
        EXPECT_TRUE(emu->memory.try_read_memory(CACHE_BASE, probe.data(), probe.size()));
    }

    TEST(DyldCachePager, RunsTheChunkFixupBeforeMapping)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_write)};

        uint64_t seen_address = 0;
        pager.set_chunk_fixup([&](const uint64_t address, const std::span<std::byte> data, sogen::memory_permission) {
            seen_address = address;
            std::ranges::fill(data, std::byte{0xAB});
        });

        ASSERT_TRUE(pager.page_in(CACHE_BASE + 0x40));

        EXPECT_EQ(seen_address, CACHE_BASE);

        std::array<std::byte, 4> guest{};
        ASSERT_TRUE(emu->memory.try_read_memory(CACHE_BASE + 0x40, guest.data(), guest.size()));
        EXPECT_EQ(guest[0], std::byte{0xAB});
        EXPECT_EQ(guest[3], std::byte{0xAB});
    }

    TEST(DyldCachePager, ClampsAChunkToTheEndOfItsRange)
    {
        const auto emu = macos_test::make_emulator();
        const uint64_t odd_size = sogen::MACOS_CACHE_CHUNK_SIZE + 0x4000;
        vector_range_reader reader{make_backing_data(odd_size)};

        std::vector<sogen::dyld_cache_backing_range> ranges{sogen::dyld_cache_backing_range{
            .address = CACHE_BASE,
            .size = odd_size,
            .path = "/fake/dyld_shared_cache_arm64e",
            .file_offset = 0,
            .permissions = sogen::memory_permission::read_exec,
        }};

        sogen::dyld_cache_pager pager{emu->memory, reader, std::move(ranges)};

        ASSERT_TRUE(pager.page_in(CACHE_BASE + sogen::MACOS_CACHE_CHUNK_SIZE));
        EXPECT_EQ(pager.resident_bytes(), 0x4000ULL);
    }

    TEST(DyldCachePager, DropsDegenerateRangesWhenBuilding)
    {
        const auto emu = macos_test::make_emulator();

        std::vector<sogen::dyld_cache_backing_range> ranges{
            sogen::dyld_cache_backing_range{.address = CACHE_BASE, .size = 0, .path = "/a", .file_offset = 0},
            sogen::dyld_cache_backing_range{.address = 0xFFFFFFFFFFFFF000ULL, .size = 0x8000, .path = "/b", .file_offset = 0},
        };

        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, std::move(ranges)};

        EXPECT_EQ(pager.span_start(), pager.span_end());
        EXPECT_FALSE(pager.page_in(CACHE_BASE));
        EXPECT_FALSE(pager.page_in(0xFFFFFFFFFFFFF000ULL));
    }

    // Taken from the real cache, where 33 of 34 regions do not start on a 2 MiB boundary and 27 pairs of
    // regions share one: 0x1f8074000 is read-only and 0x1f807c000 is read+execute, both inside the chunk
    // beginning at 0x1f8000000. A pager keyed on the aligned chunk address would report the second as
    // already resident on the strength of the first, map nothing, and hand the guest a hole -- with the
    // wrong permissions on the part it did map.
    TEST(DyldCachePager, TwoRegionsSharingAChunkAreBothMapped)
    {
        const auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(4 * sogen::MACOS_CACHE_CHUNK_SIZE);
        vector_range_reader reader{backing};

        constexpr uint64_t first = CACHE_BASE + 0x74000;
        constexpr uint64_t second = CACHE_BASE + 0x7c000;

        std::vector<sogen::dyld_cache_backing_range> ranges{
            sogen::dyld_cache_backing_range{.address = first,
                                            .size = 0x8000,
                                            .path = "/fake/cache",
                                            .file_offset = 0x74000,
                                            .permissions = sogen::memory_permission::read},
            sogen::dyld_cache_backing_range{.address = second,
                                            .size = 0x4000,
                                            .path = "/fake/cache",
                                            .file_offset = 0x7c000,
                                            .permissions = sogen::memory_permission::read_exec},
        };

        sogen::dyld_cache_pager pager{emu->memory, reader, std::move(ranges)};

        ASSERT_TRUE(pager.page_in(first));
        ASSERT_TRUE(pager.page_in(second)) << "the second region shares a 2 MiB chunk with the first";

        EXPECT_EQ(pager.paged_in_chunks(), 2ULL) << "the second region was never paged in";
        EXPECT_EQ(pager.resident_bytes(), 0x8000ULL + 0x4000ULL);

        std::array<std::byte, 4> guest{};
        ASSERT_TRUE(emu->memory.try_read_memory(second, guest.data(), guest.size()));

        const auto expected = std::span{backing}.subspan(0x7c000, guest.size());
        EXPECT_EQ(std::memcmp(guest.data(), expected.data(), guest.size()), 0) << "the second region holds the wrong bytes";
    }

    // Its own permissions, not the neighbour's: mapping cache TEXT as read-only would fault the guest on
    // its first call into the cache, and mapping DATA as executable would be worse.
    TEST(DyldCachePager, EachRegionKeepsItsOwnPermissions)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(4 * sogen::MACOS_CACHE_CHUNK_SIZE)};

        constexpr uint64_t writable = CACHE_BASE + 0x10000;
        constexpr uint64_t executable = CACHE_BASE + 0x18000;

        std::vector<sogen::dyld_cache_backing_range> ranges{
            sogen::dyld_cache_backing_range{.address = writable,
                                            .size = 0x8000,
                                            .path = "/fake/cache",
                                            .file_offset = 0x10000,
                                            .permissions = sogen::memory_permission::read_write},
            sogen::dyld_cache_backing_range{.address = executable,
                                            .size = 0x8000,
                                            .path = "/fake/cache",
                                            .file_offset = 0x18000,
                                            .permissions = sogen::memory_permission::read_exec},
        };

        sogen::dyld_cache_pager pager{emu->memory, reader, std::move(ranges)};

        ASSERT_TRUE(pager.page_in(writable));
        ASSERT_TRUE(pager.page_in(executable));

        const auto& regions = emu->memory.get_mapped_regions();
        ASSERT_TRUE(regions.contains(writable));
        ASSERT_TRUE(regions.contains(executable));
        EXPECT_EQ(regions.at(writable).permissions, sogen::memory_permission::read_write);
        EXPECT_EQ(regions.at(executable).permissions, sogen::memory_permission::read_exec);
    }

    TEST(DyldCachePagerHook, GuestLoadFaultsTheChunkIn)
    {
        auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(REGION_SIZE);
        vector_range_reader reader{backing};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec));
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        constexpr uint64_t code_base = 0x100000000ULL;
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2B00001, // movz x1, #0x8000, lsl #16
                                         0xF2C00021, // movk x1, #1, lsl #32   -> x1 = 0x180000000
                                         0xF9400020, // ldr x0, [x1]
                                         0xD4200000, // brk #0
                                     });

        emu->start(4);

        EXPECT_EQ(raw->paged_in_chunks(), 1ULL);

        uint64_t expected = 0;
        std::memcpy(&expected, backing.data(), sizeof(expected));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), expected);
    }

    TEST(DyldCachePagerHook, FaultOutsideTheCacheStillStops)
    {
        auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec));
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        constexpr uint64_t code_base = 0x100000000ULL;
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2A80001, // movz x1, #0x4000, lsl #16 -> x1 = 0x40000000
                                         0xF9400020, // ldr x0, [x1]
                                         0xD4200000, // brk #0
                                     });

        emu->start(3);

        EXPECT_EQ(raw->paged_in_chunks(), 0ULL);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::none);

        // The pager has to decline a fault it has no business handling, not fail at it. Claiming the
        // page-in failed would blame the cache for an access nowhere near it, and that message is the
        // first thing anyone reads when working out why a run stopped.
        EXPECT_EQ(emu->last_stop_detail().find("dyld cache"), std::string::npos)
            << "reported as a cache page-in failure: " << emu->last_stop_detail();
    }

    TEST(DyldCachePagerHook, EmulatorOwnsThePagerAfterInstall)
    {
        auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};

        EXPECT_EQ(emu->cache_pager, nullptr);

        sogen::install_dyld_cache_pager(
            *emu, std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)));

        ASSERT_NE(emu->cache_pager, nullptr);
        EXPECT_TRUE(emu->cache_pager->covers(CACHE_BASE));
    }

    // A store into cache DATA has to reach the paged-in chunk, not just avoid faulting. Cache DATA is
    // where dyld writes every rebased pointer, so a write that silently went nowhere would leave the
    // guest reading unrelocated addresses.
    TEST(DyldCachePagerHook, GuestStoreFaultsTheChunkInAndLands)
    {
        auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_write));
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        constexpr uint64_t code_base = 0x100000000ULL;
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2B00001, // movz x1, #0x8000, lsl #16
                                         0xF2C00021, // movk x1, #1, lsl #32   -> x1 = 0x180000000
                                         0xD2822460, // movz x0, #0x1123
                                         0xF9000020, // str x0, [x1]
                                         0xD4200000, // brk #0
                                     });

        emu->start(5);

        EXPECT_EQ(raw->paged_in_chunks(), 1ULL);

        uint64_t stored = 0;
        ASSERT_TRUE(emu->memory.try_read_memory(CACHE_BASE, &stored, sizeof(stored)));
        EXPECT_EQ(stored, 0x1123ULL) << "the store did not reach the chunk the fault paged in";
    }

    // Faulting the same chunk back in after eviction must produce the file's bytes again, not whatever
    // the guest had left there -- the eviction released the mapping, and only the file has the content.
    TEST(DyldCachePagerHook, AnEvictedChunkFaultsBackInWithItsOriginalBytes)
    {
        auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(REGION_SIZE);
        vector_range_reader reader{backing};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec),
                                                               2 * sogen::MACOS_CACHE_CHUNK_SIZE);
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        ASSERT_TRUE(raw->page_in(CACHE_BASE));
        ASSERT_TRUE(raw->page_in(CACHE_BASE + 1 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_TRUE(raw->page_in(CACHE_BASE + 2 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_EQ(raw->evicted_chunks(), 1ULL);

        // Asked of the mapping rather than by reading: a host read materialises what it cannot find.
        ASSERT_FALSE(emu->memory.overlaps_mapped_region(CACHE_BASE, sizeof(uint64_t)));

        constexpr uint64_t code_base = 0x100000000ULL;
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2B00001, // movz x1, #0x8000, lsl #16
                                         0xF2C00021, // movk x1, #1, lsl #32   -> x1 = 0x180000000
                                         0xF9400020, // ldr x0, [x1]
                                         0xD4200000, // brk #0
                                     });

        emu->start(4);

        uint64_t expected = 0;
        std::memcpy(&expected, backing.data(), sizeof(expected));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), expected);
        EXPECT_EQ(raw->paged_in_chunks(), 4ULL);
    }

    // Only a guest access faults. A host-side read -- the GUI interception reading an objc class name
    // out of the cache to recognise a CASDFFillEffect -- would otherwise see an absent chunk as absent
    // memory and silently drop what it was inspecting.
    TEST(DyldCachePagerHook, AHostReadMaterialisesAnAbsentChunk)
    {
        auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(REGION_SIZE);
        vector_range_reader reader{backing};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec));
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        ASSERT_FALSE(emu->memory.overlaps_mapped_region(CACHE_BASE, sizeof(uint64_t)));

        uint64_t value = 0;
        ASSERT_TRUE(emu->memory.try_read_memory(CACHE_BASE, &value, sizeof(value)));
        EXPECT_EQ(raw->paged_in_chunks(), 1ULL);

        uint64_t expected = 0;
        std::memcpy(&expected, backing.data(), sizeof(expected));
        EXPECT_EQ(value, expected);
    }

    // The chunk the first byte lands in is not the only one the read needs.
    TEST(DyldCachePagerHook, AHostReadStraddlingTwoChunksMaterialisesBoth)
    {
        auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(REGION_SIZE);
        vector_range_reader reader{backing};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec));
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        const auto seam = CACHE_BASE + sogen::MACOS_CACHE_CHUNK_SIZE - 4;

        std::array<std::byte, 8> value{};
        ASSERT_TRUE(emu->memory.try_read_memory(seam, value.data(), value.size()));
        EXPECT_EQ(raw->paged_in_chunks(), 2ULL);

        const auto offset = static_cast<size_t>(seam - CACHE_BASE);
        for (size_t i = 0; i < value.size(); ++i)
        {
            EXPECT_EQ(value[i], backing[offset + i]) << "at " << i;
        }
    }

    // A write into cache TEXT is a protection violation, not a missing page. The chunk is already
    // resident, so a pager that treats every fault as its own would "handle" it by touching the LRU and
    // resuming -- and the guest would retry the same store forever, which reads as a hang rather than the
    // permission error it is.
    TEST(DyldCachePagerHook, AWriteToAResidentReadOnlyChunkStopsRatherThanLooping)
    {
        auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec));
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        ASSERT_TRUE(raw->page_in(CACHE_BASE));
        const auto paged_in_before = raw->paged_in_chunks();

        constexpr uint64_t code_base = 0x100000000ULL;
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2B00001, // movz x1, #0x8000, lsl #16
                                         0xF2C00021, // movk x1, #1, lsl #32   -> x1 = 0x180000000
                                         0xD2822460, // movz x0, #0x1123
                                         0xF9000020, // str x0, [x1]
                                         0xD4200000, // brk #0
                                     });

        emu->start(5);

        EXPECT_EQ(raw->paged_in_chunks(), paged_in_before) << "a protection fault paged something in";
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_memory_violation);

        uint64_t stored = 0;
        ASSERT_TRUE(emu->memory.try_read_memory(CACHE_BASE, &stored, sizeof(stored)));
        EXPECT_NE(stored, 0x1123ULL) << "the store reached a chunk mapped without write permission";
    }

    // covers() saying yes does not mean page_in() will succeed: the chunk it wants can collide with
    // something already mapped, which is what a wrong slide looks like from here. Resuming on that would
    // tell the backend the fault was handled while nothing was mapped, and the guest would re-fault on
    // the same address for as long as it was allowed to run.
    TEST(DyldCachePagerHook, APageInThatCannotMapReportsItInsteadOfResuming)
    {
        auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec));
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        // Occupies part of the chunk the fault below will try to page in.
        ASSERT_TRUE(emu->memory.allocate_memory(CACHE_BASE, 0x4000, sogen::memory_permission::read_write));

        constexpr uint64_t probe = CACHE_BASE + 0x8000;
        ASSERT_TRUE(raw->covers(probe));
        ASSERT_FALSE(raw->page_in(probe)) << "the collision this test depends on did not happen";

        constexpr uint64_t code_base = 0x100000000ULL;
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2B00001, // movz x1, #0x8000, lsl #16
                                         0xF2C00021, // movk x1, #1, lsl #32   -> x1 = 0x180000000
                                         0xF2900001, // movk x1, #0x8000       -> x1 = 0x180008000
                                         0xF9400020, // ldr x0, [x1]
                                         0xD4200000, // brk #0
                                     });

        emu->start(5);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_memory_violation);
        EXPECT_NE(emu->last_stop_detail().find("dyld cache page-in failed"), std::string::npos)
            << "stopped without saying the cache page-in was what failed: " << emu->last_stop_detail();
    }

    // The pager has to be able to read the cache while it is handling a fault, without materialising
    // anything: the slide blob lives in the read-only region, and reading it through guest memory during
    // a page-in would fault re-entrantly.
    TEST(DyldCachePager, ReadsBackingWithoutMaterialisingAnything)
    {
        const auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(REGION_SIZE);
        vector_range_reader reader{backing};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)};

        std::array<std::byte, 64> got{};
        constexpr uint64_t at = CACHE_BASE + 3 * sogen::MACOS_CACHE_CHUNK_SIZE + 0x1234;
        ASSERT_TRUE(pager.read_backing(at, got));

        EXPECT_EQ(pager.paged_in_chunks(), 0ULL) << "reading the backing materialised a chunk";
        EXPECT_EQ(pager.resident_bytes(), 0ULL);

        const auto expected = std::span{backing}.subspan(static_cast<size_t>(at - CACHE_BASE), got.size());
        EXPECT_EQ(std::memcmp(got.data(), expected.data(), got.size()), 0);

        std::array<std::byte, 4> unmapped{};
        EXPECT_FALSE(emu->memory.try_read_memory(at, unmapped.data(), unmapped.size()));
    }

    TEST(DyldCachePager, ReadingBackingOutsideEveryRangeFails)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)};

        std::array<std::byte, 8> got{};
        EXPECT_FALSE(pager.read_backing(CACHE_BASE + REGION_SIZE, got));
        EXPECT_FALSE(pager.read_backing(0, got));

        // Straddling the end is a partial answer, which is worse than no answer: the caller would parse
        // whatever the tail happened to hold.
        EXPECT_FALSE(pager.read_backing(CACHE_BASE + REGION_SIZE - 4, got));

        EXPECT_TRUE(pager.read_backing(CACHE_BASE, {})) << "an empty read asks for nothing and gets it";
    }

    // Ranges that touch in memory come from different file offsets, so a read crossing the seam has to
    // be split rather than served from one file position.
    TEST(DyldCachePager, ReadsBackingAcrossTwoAdjacentRanges)
    {
        const auto emu = macos_test::make_emulator();
        const auto backing = make_backing_data(4 * sogen::MACOS_CACHE_CHUNK_SIZE);
        vector_range_reader reader{backing};

        constexpr uint64_t first_size = 0x8000;
        constexpr uint64_t second_offset = 0x100000;

        std::vector<sogen::dyld_cache_backing_range> ranges{
            sogen::dyld_cache_backing_range{.address = CACHE_BASE,
                                            .size = first_size,
                                            .path = "/fake/cache",
                                            .file_offset = 0,
                                            .permissions = sogen::memory_permission::read},
            sogen::dyld_cache_backing_range{.address = CACHE_BASE + first_size,
                                            .size = 0x8000,
                                            .path = "/fake/cache",
                                            .file_offset = second_offset,
                                            .permissions = sogen::memory_permission::read},
        };

        sogen::dyld_cache_pager pager{emu->memory, reader, std::move(ranges)};

        std::array<std::byte, 16> got{};
        ASSERT_TRUE(pager.read_backing(CACHE_BASE + first_size - 8, got));

        for (size_t i = 0; i < 8; ++i)
        {
            EXPECT_EQ(got[i], backing[static_cast<size_t>(first_size - 8 + i)]) << "before the seam, at " << i;
        }
        for (size_t i = 8; i < got.size(); ++i)
        {
            EXPECT_EQ(got[i], backing[static_cast<size_t>(second_offset + i - 8)]) << "after the seam, at " << i;
        }
    }

    TEST(DyldCachePager, PinRefusesWhatIsNotResident)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec)};

        EXPECT_FALSE(pager.pin(CACHE_BASE)) << "nothing is resident yet";
        EXPECT_FALSE(pager.pin(CACHE_BASE + REGION_SIZE)) << "outside every range";

        ASSERT_TRUE(pager.page_in(CACHE_BASE));
        EXPECT_TRUE(pager.pin(CACHE_BASE));
    }

    // A patched chunk holds bytes the backing file does not, same as a writable one. Without a pin the
    // LRU would evict it under pressure, and the re-fault would restore the file's unpatched bytes.
    TEST(DyldCachePager, APinnedChunkSurvivesEviction)
    {
        const auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};
        sogen::dyld_cache_pager pager{emu->memory, reader, make_ranges(sogen::memory_permission::read_exec),
                                      2 * sogen::MACOS_CACHE_CHUNK_SIZE};

        ASSERT_TRUE(pager.page_in(CACHE_BASE));
        ASSERT_TRUE(pager.pin(CACHE_BASE));

        ASSERT_TRUE(pager.page_in(CACHE_BASE + 1 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_TRUE(pager.page_in(CACHE_BASE + 2 * sogen::MACOS_CACHE_CHUNK_SIZE));

        EXPECT_EQ(pager.evicted_chunks(), 1ULL);

        std::array<std::byte, 4> probe{};
        EXPECT_TRUE(emu->memory.try_read_memory(CACHE_BASE, probe.data(), probe.size())) << "the pinned chunk was evicted anyway";
        EXPECT_FALSE(emu->memory.try_read_memory(CACHE_BASE + 1 * sogen::MACOS_CACHE_CHUNK_SIZE, probe.data(), probe.size()));
    }

    // The browser installs no host mapping for the cache, so a host-side patch of a cache export has to
    // pull the page in through the pager first -- a guest fault would do it, but the patch runs before
    // any guest code touches the export.
    TEST(DyldCachePagerPatch, PatchNativeEntryMaterialisesTheChunkAndKeepsIt)
    {
        auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec),
                                                               2 * sogen::MACOS_CACHE_CHUNK_SIZE);
        auto* raw = pager.get();
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        constexpr uint64_t entry = CACHE_BASE + 0x1234;

        ASSERT_FALSE(emu->memory.overlaps_mapped_region(entry, sizeof(uint32_t)))
            << "the fixture only works if the export's chunk starts unmaterialised";

        uint32_t word = 0;

        ASSERT_TRUE(sogen::patch_native_entry(*emu, entry));
        EXPECT_EQ(raw->paged_in_chunks(), 1ULL);

        ASSERT_TRUE(emu->memory.try_read_memory(entry, &word, sizeof(word)));
        EXPECT_EQ(word, sogen::MACOS_ARM64_SVC_80);

        ASSERT_TRUE(raw->page_in(CACHE_BASE + 1 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_TRUE(raw->page_in(CACHE_BASE + 2 * sogen::MACOS_CACHE_CHUNK_SIZE));
        ASSERT_GE(raw->evicted_chunks(), 1ULL) << "the budget forced no eviction; the test proves nothing";

        ASSERT_TRUE(emu->memory.try_read_memory(entry, &word, sizeof(word)))
            << "the patched chunk was evicted; a re-fault would restore the file's bytes without the trap";
        EXPECT_EQ(word, sogen::MACOS_ARM64_SVC_80);
    }

    // The same force-fetch must fail honestly: an unmappable chunk (here: a colliding mapping) makes the
    // patch report failure rather than skip silently.
    TEST(DyldCachePagerPatch, PatchNativeEntryFailsWhenTheChunkCannotBeMapped)
    {
        auto emu = macos_test::make_emulator();
        vector_range_reader reader{make_backing_data(REGION_SIZE)};

        auto pager = std::make_unique<sogen::dyld_cache_pager>(emu->memory, reader, make_ranges(sogen::memory_permission::read_exec));
        sogen::install_dyld_cache_pager(*emu, std::move(pager));

        ASSERT_TRUE(emu->memory.allocate_memory(CACHE_BASE, sogen::MACOS_CACHE_CHUNK_SIZE, sogen::memory_permission::read_write));

        EXPECT_FALSE(sogen::patch_native_entry(*emu, CACHE_BASE + 0x1234));
    }
}
