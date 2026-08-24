#include <gtest/gtest.h>
#include <memory_manager.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    namespace
    {
        // Stands in for a backend sharing the guest address space with the host process (FEX on Apple
        // Silicon). Everything the memory_interface base does not need for that is left stubbed: the code
        // under test only queries reserved_host_ranges[_in] and never touches guest memory.
        class fake_host_memory : public memory_interface
        {
          public:
            // Reported by both the full reserved_host_ranges() scan and the windowed probe.
            std::vector<host_reserved_range> foreign_ranges{};

            // Reported only by the windowed probe. A backend may skip ranges it considers guest-owned in
            // its full scan while its windowed probe still reports them occupied, which is why
            // find_free_host_allocation_base's rescan also records the rejected pick window - a full-only
            // rescan would re-pick these identically every iteration.
            std::vector<host_reserved_range> hidden_from_full_scan{};

            std::vector<host_reserved_range> claimed_ranges{};
            std::vector<host_reserved_range> released_ranges{};
            bool requires_host_identity{};
            std::optional<uint64_t> mapped_host_address{};
            void* mapped_host_pointer{};

            void reserve_guest_address_range(const uint64_t address, const size_t size) override
            {
                this->claimed_ranges.push_back({.address = address, .size = size});
            }

            void release_guest_address_range(const uint64_t address, const size_t size) override
            {
                this->released_ranges.push_back({.address = address, .size = size});
            }

            std::vector<host_reserved_range> reserved_host_ranges() const override
            {
                return this->foreign_ranges;
            }

            std::vector<host_reserved_range> reserved_host_ranges_in(const uint64_t address, const size_t size) const override
            {
                std::vector<host_reserved_range> result{};
                const auto window_end = address + size;
                const auto clip = [&](const std::vector<host_reserved_range>& ranges) {
                    for (const auto& range : ranges)
                    {
                        const auto range_end = range.address + range.size;
                        const auto start = std::max(address, range.address);
                        const auto end = std::min(window_end, range_end);
                        if (start < end)
                        {
                            result.push_back({.address = start, .size = static_cast<size_t>(end - start)});
                        }
                    }
                };
                clip(this->foreign_ranges);
                clip(this->hidden_from_full_scan);
                return result;
            }

            void read_memory(uint64_t, void*, size_t) const override
            {
                throw std::logic_error("unexpected read_memory in host-allocation test");
            }

            bool try_read_memory(uint64_t, void*, size_t) const override
            {
                return false;
            }

            void write_memory(uint64_t, const void*, size_t) override
            {
                throw std::logic_error("unexpected write_memory in host-allocation test");
            }

            bool try_write_memory(uint64_t, const void*, size_t) override
            {
                return false;
            }

          private:
            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
            }

            void map_memory(uint64_t, size_t, memory_permission) override
            {
            }

            void map_host_memory(const uint64_t address, size_t, void* host_pointer, memory_permission) override
            {
                this->mapped_host_address = address;
                this->mapped_host_pointer = host_pointer;
            }

            bool host_memory_mapping_requires_identity() const override
            {
                return this->requires_host_identity;
            }

            void unmap_memory(uint64_t, size_t) override
            {
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }
        };
    }

    // Covers the helper the module-relocation fallback relies on: a bookkeeping-only pick can already
    // be occupied by a foreign host mapping on backends sharing the guest VA with the host process.
    TEST(HostAllocationTest, FindFreeHostBaseSkipsForeignOccupiedPick)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        const uint64_t naive_base = mm.find_free_allocation_base(size, start);
        ASSERT_NE(naive_base, 0u);

        host.foreign_ranges.push_back({.address = naive_base, .size = size});

        // The plain pick cannot see the foreign mapping and still returns the occupied address.
        ASSERT_EQ(mm.find_free_allocation_base(size, start), naive_base);

        const uint64_t host_base = mm.find_free_host_allocation_base(size, start);
        ASSERT_NE(host_base, 0u);
        ASSERT_NE(host_base, naive_base);
        ASSERT_GE(host_base, naive_base + size);
        ASSERT_TRUE(mm.host_window_is_free(host_base, size));
    }

    // Backends with an independent guest address space report no foreign ranges, so the host-aware pick
    // must stay a no-op there.
    TEST(HostAllocationTest, FindFreeHostBaseMatchesPlainWhenNoForeignRanges)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        ASSERT_EQ(mm.find_free_host_allocation_base(size, start), mm.find_free_allocation_base(size, start));
    }

    TEST(HostAllocationTest, AllocateHostMemoryUsesSourceAddressWhenBackendRequiresIdentity)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t address = DEFAULT_ALLOCATION_ADDRESS_64BIT;
        host.foreign_ranges.push_back({.address = address, .size = size});
        host.requires_host_identity = true;
        mm.reserve_host_memory_ranges();

        ASSERT_EQ(mm.allocate_host_memory(size, reinterpret_cast<void*>(address), memory_permission::read_write), address);
        ASSERT_EQ(host.mapped_host_address, address);
        ASSERT_EQ(mm.get_region_kind(address), memory_region_kind::mmio);

        mm.reset_host_memory_ranges();
        ASSERT_EQ(mm.get_region_kind(address), memory_region_kind::mmio);
    }

#ifdef __ANDROID__
    TEST(HostAllocationTest, AllocateHostMemoryUntagsSourceAddressOnAndroid)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t address = DEFAULT_ALLOCATION_ADDRESS_64BIT;
        constexpr uint64_t tag = 0xAB00000000000000;
        void* const tagged_pointer = reinterpret_cast<void*>(address | tag);
        host.foreign_ranges.push_back({.address = address, .size = size});
        host.requires_host_identity = true;
        mm.reserve_host_memory_ranges();

        ASSERT_EQ(mm.allocate_host_memory(size, tagged_pointer, memory_permission::read_write), address);
        ASSERT_EQ(host.mapped_host_address, address);
        ASSERT_EQ(host.mapped_host_pointer, tagged_pointer);
        ASSERT_EQ(mm.get_region_kind(address), memory_region_kind::mmio);
    }
#endif

    TEST(HostAllocationTest, AllocateHostMemoryClaimsBackendSelectedAddress)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        const uint64_t address = mm.allocate_host_memory(size, reinterpret_cast<void*>(0x70000000), memory_permission::read_write);

        ASSERT_NE(address, 0u);
        ASSERT_EQ(host.mapped_host_address, address);
        ASSERT_EQ(host.claimed_ranges.size(), 1u);
        ASSERT_EQ(host.claimed_ranges.front().address, address);
        ASSERT_EQ(host.claimed_ranges.front().size, size);
    }

    // Pins the windowed half of find_free_host_allocation_base's rescan: for a range the full scan
    // omits, a full-only rescan records nothing new and the same occupied pick is chosen every
    // iteration until the retry cap.
    TEST(HostAllocationTest, FindFreeHostBaseStepsPastRangeHiddenFromFullScan)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        const uint64_t naive_base = mm.find_free_allocation_base(size, start);
        ASSERT_NE(naive_base, 0u);

        host.hidden_from_full_scan.push_back({.address = naive_base, .size = size});

        const uint64_t host_base = mm.find_free_host_allocation_base(size, start);
        ASSERT_NE(host_base, 0u);
        ASSERT_GE(host_base, naive_base + size);
        ASSERT_TRUE(mm.host_window_is_free(host_base, size));
    }

    // Pins the other half: a contiguous foreign region wider than the retry budget's worth of picks can
    // only be cleared if the full scan records the whole region at once, since one pick-sized slice per
    // iteration would exhaust the retries mid-region.
    TEST(HostAllocationTest, FindFreeHostBaseJumpsPastLargeForeignRegionWithinRetryBudget)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        const uint64_t naive_base = mm.find_free_allocation_base(size, start);
        ASSERT_NE(naive_base, 0u);

        constexpr size_t large_region = size * 4096;
        host.foreign_ranges.push_back({.address = naive_base, .size = large_region});

        const uint64_t host_base = mm.find_free_host_allocation_base(size, start);
        ASSERT_NE(host_base, 0u);
        ASSERT_GE(host_base, naive_base + large_region);
        ASSERT_TRUE(mm.host_window_is_free(host_base, size));
    }

    // allocate_mmio may claim an address inside a recorded host_reserved range (the KUSD MMIO page lives
    // inside the __PAGEZERO carve-out on FEX/Apple). If the entries nested, a query above the small MMIO
    // entry would see only that entry as its predecessor and miss the larger host_reserved range still
    // covering the queried address, so the coverage must be split around the hole instead.
    TEST(HostAllocationTest, OverlapQuerySeesHostReservedCoverageAroundNestedMmio)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr uint64_t reserved_base = 0x70000000;
        constexpr size_t reserved_size = 0x20000000;
        constexpr uint64_t mmio_base = 0x7ffe0000;
        constexpr size_t mmio_size = 0x1000;

        host.foreign_ranges.push_back({.address = reserved_base, .size = reserved_size});
        mm.reserve_host_memory_ranges();

        ASSERT_TRUE(mm.allocate_mmio(mmio_base, mmio_size, [](uint64_t, void*, size_t) {}, [](uint64_t, const void*, size_t) {}));

        ASSERT_EQ(mm.get_region_kind(mmio_base), memory_region_kind::mmio);
        ASSERT_EQ(mm.get_region_kind(mmio_base - 1), memory_region_kind::host_reserved);
        ASSERT_EQ(mm.get_region_kind(mmio_base + mmio_size), memory_region_kind::host_reserved);

        ASSERT_TRUE(mm.overlaps_reserved_region(0x80000000, 0x1000));
        ASSERT_TRUE(mm.overlaps_reserved_region(mmio_base, mmio_size));
        ASSERT_TRUE(mm.overlaps_reserved_region(reserved_base, 0x1000));
        ASSERT_FALSE(mm.overlaps_reserved_region(reserved_base + reserved_size, 0x1000));
    }

    // carve_host_reserved_hole drops a host_reserved range's tracking entry wholesale when splitting it,
    // so it assumes nothing is ever committed inside one. A commit at an explicit base within the range
    // would populate committed_regions and a later carve would silently orphan that mapping.
    TEST(HostAllocationTest, CommitIntoHostReservedRegionIsRejected)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr uint64_t reserved_base = 0x70000000;
        constexpr size_t reserved_size = 0x20000000;

        host.foreign_ranges.push_back({.address = reserved_base, .size = reserved_size});
        mm.reserve_host_memory_ranges();
        ASSERT_EQ(mm.get_region_kind(reserved_base), memory_region_kind::host_reserved);

        constexpr uint64_t commit_base = reserved_base + 0x100000;
        ASSERT_FALSE(mm.commit_memory(commit_base, 0x1000, nt_memory_permission{memory_permission::read_write}));

        const auto& regions = mm.get_reserved_regions();
        const auto entry = regions.find(reserved_base);
        ASSERT_NE(entry, regions.end());
        ASSERT_TRUE(entry->second.committed_regions.empty());
    }

    // A decommitted range stays MEM_RESERVE'd, so its host claim must persist - a foreign host
    // allocation landing there would be clobbered by a later recommit. Only a genuine release hands the
    // claim back.
    TEST(HostAllocationTest, ReleaseNotifiesHostClaimReleaseButDecommitDoesNot)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x11000;
        const uint64_t base = mm.allocate_memory(size, nt_memory_permission{memory_permission::read_write});
        ASSERT_NE(base, 0u);

        ASSERT_EQ(host.claimed_ranges.size(), 1u);
        ASSERT_EQ(host.claimed_ranges[0].address, base);
        ASSERT_EQ(host.claimed_ranges[0].size, size);

        ASSERT_TRUE(mm.decommit_memory(base, size));
        ASSERT_TRUE(host.released_ranges.empty());

        ASSERT_TRUE(mm.release_memory(base, 0));
        ASSERT_EQ(host.released_ranges.size(), 1u);
        ASSERT_LE(host.released_ranges[0].address, base);
        ASSERT_GE(host.released_ranges[0].address + host.released_ranges[0].size, base + size);
    }

    // A reserve-only allocation never reaches map_memory, so the host-level claim is the only thing
    // keeping the host's own allocator out of the range until the guest commits - and the commit's
    // MAP_FIXED would clobber whatever landed there meanwhile.
    TEST(HostAllocationTest, FixedBaseReserveOnlyAllocationClaimsHostRange)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr uint64_t base = 0x40000000;
        constexpr size_t size = 0x10000;

        ASSERT_TRUE(mm.allocate_memory(base, size, nt_memory_permission{memory_permission::read_write}, true));

        ASSERT_EQ(host.claimed_ranges.size(), 1u);
        ASSERT_EQ(host.claimed_ranges[0].address, base);
        ASSERT_EQ(host.claimed_ranges[0].size, size);

        ASSERT_TRUE(mm.release_memory(base, 0));
        ASSERT_EQ(host.released_ranges.size(), 1u);
    }

    // Exercises reserve_host_memory_ranges_in's existing guard at the top of allocate_memory, not the
    // reserve-only host claim tested above: a fixed-address request racing a foreign mapping in after
    // find_free_allocation_base/host_window_is_free confirmed the address free must still fail cleanly.
    TEST(HostAllocationTest, ForeignMappingArrivingAfterTheProbeFailsTheAllocationWithoutClaiming)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        const uint64_t confirmed_base = mm.find_free_allocation_base(size, start);
        ASSERT_NE(confirmed_base, 0u);
        ASSERT_TRUE(mm.host_window_is_free(confirmed_base, size));

        host.foreign_ranges.push_back({.address = confirmed_base, .size = size});

        ASSERT_FALSE(mm.allocate_memory(confirmed_base, size, nt_memory_permission{memory_permission::read_write}, true));
        ASSERT_TRUE(host.claimed_ranges.empty());
        ASSERT_EQ(mm.get_region_kind(confirmed_base), memory_region_kind::host_reserved);
    }
} // namespace sogen::test
