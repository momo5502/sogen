#include <gtest/gtest.h>
#include <memory_manager.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    namespace
    {
        // Minimal memory_interface reporting a controllable set of "foreign" host ranges - the ranges a
        // backend sharing the guest address space with the host process (FEX on Apple Silicon: guest VA ==
        // host VA) would say the guest must avoid because the host itself already occupies them. Everything
        // else is an unused stub: the code under test (find_free_host_allocation_base) only ever queries
        // reserved_host_ranges[_in]; it never maps, reads or writes guest memory (its allocate_memory_raw
        // calls are all reserve-only).
        class fake_host_memory : public memory_interface
        {
          public:
            // Foreign host mappings the guest must avoid. Reported by BOTH the full reserved_host_ranges()
            // scan and the windowed reserved_host_ranges_in() probe - an ordinary foreign occupant.
            std::vector<host_reserved_range> foreign_ranges{};

            // Ranges the windowed reserved_host_ranges_in() probe reports as occupied but the full
            // reserved_host_ranges() scan deliberately OMITS - a backend may skip ranges it considers
            // guest-owned rather than foreign in its full scan while its windowed probe still reports
            // them occupied. This asymmetry is why find_free_host_allocation_base's rescan must include
            // the windowed reserve_host_memory_ranges_in(pick): a full-only rescan never records these,
            // so a pick that lands here is re-picked identically every iteration until the retry cap.
            std::vector<host_reserved_range> hidden_from_full_scan{};

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

            void unmap_memory(uint64_t, size_t) override
            {
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }
        };
    }

    // Regression coverage for the module-relocation fallback's host-collision recovery. Before the fix,
    // map_module_from_data's relocation fallback picked a base with find_free_allocation_base (sogen's own
    // bookkeeping only) and retried the map exactly once; if that pick was already occupied by a foreign
    // host mapping - possible on backends sharing the guest VA with the host process (FEX on Apple Silicon)
    // where any real host allocation can land on a guest-owned VA - it threw "Memory range not allocatable".
    // The fallback now routes through find_free_host_allocation_base, which confirms the pick is actually
    // free at the host level and re-picks past any foreign occupant. This exercises that shared helper.
    TEST(HostAllocationTest, FindFreeHostBaseSkipsForeignOccupiedPick)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        // The address the plain, bookkeeping-only pick hands back - the one the old fallback used.
        const uint64_t naive_base = mm.find_free_allocation_base(size, start);
        ASSERT_NE(naive_base, 0u);

        // A foreign host mapping now occupies exactly that address, invisible to sogen's bookkeeping.
        host.foreign_ranges.push_back({.address = naive_base, .size = size});

        // The plain pick still returns the now-occupied address (it cannot see the foreign mapping)...
        ASSERT_EQ(mm.find_free_allocation_base(size, start), naive_base);

        // ...but the host-aware pick confirms and steps past it.
        const uint64_t host_base = mm.find_free_host_allocation_base(size, start);
        ASSERT_NE(host_base, 0u);
        ASSERT_NE(host_base, naive_base);
        ASSERT_GE(host_base, naive_base + size);

        // The returned base really is clear of every foreign range.
        ASSERT_TRUE(mm.host_window_is_free(host_base, size));
    }

    // With no foreign mappings (the default / independent-address-space backends such as unicorn), the
    // host-aware pick is identical to the plain one - the fix is a correctness no-op there.
    TEST(HostAllocationTest, FindFreeHostBaseMatchesPlainWhenNoForeignRanges)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        ASSERT_EQ(mm.find_free_host_allocation_base(size, start), mm.find_free_allocation_base(size, start));
    }

    // find_free_host_allocation_base confirms a pick with the windowed host_window_is_free probe; a
    // rescan that only used the full reserve_host_memory_ranges() would record nothing new for a range
    // the full scan omits, so the same occupied pick would be chosen every iteration until the retry
    // cap and the allocation would fail outright. The rescan therefore also records the just-rejected
    // pick window via reserve_host_memory_ranges_in, so the next pick steps past a range the full scan
    // cannot see. hidden_from_full_scan models exactly that range.
    TEST(HostAllocationTest, FindFreeHostBaseStepsPastRangeHiddenFromFullScan)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        const uint64_t naive_base = mm.find_free_allocation_base(size, start);
        ASSERT_NE(naive_base, 0u);

        // Occupy the naive pick with a range the full reserved_host_ranges() scan never reports (only the
        // windowed reserved_host_ranges_in() sees it). A full-only rescan would spin on naive_base
        // forever; the windowed record in the rescan is what lets the pick advance.
        host.hidden_from_full_scan.push_back({.address = naive_base, .size = size});

        const uint64_t host_base = mm.find_free_host_allocation_base(size, start);
        ASSERT_NE(host_base, 0u);
        ASSERT_GE(host_base, naive_base + size);
        ASSERT_TRUE(mm.host_window_is_free(host_base, size));
    }

    // The complementary need: the rescan must ALSO keep the full reserve_host_memory_ranges(), not just
    // the windowed record. A large contiguous foreign region at the naive pick - wider than
    // max_host_reserved_retries picks - can only be cleared in the bounded retry budget if the full scan
    // records the whole region at once; recording just one pick-sized slice per iteration would exhaust
    // the retries mid-region and fail. The region here is visible to the full scan and spans far more
    // than the retry budget could step past one slice at a time.
    TEST(HostAllocationTest, FindFreeHostBaseJumpsPastLargeForeignRegionWithinRetryBudget)
    {
        fake_host_memory host{};
        memory_manager mm{host};

        constexpr size_t size = 0x2000;
        constexpr uint64_t start = DEFAULT_ALLOCATION_ADDRESS_64BIT;

        const uint64_t naive_base = mm.find_free_allocation_base(size, start);
        ASSERT_NE(naive_base, 0u);

        // A single contiguous foreign region far larger than (retry budget * size) sitting on the naive pick.
        // Only a full-scan rescan (which records the whole region in one step) can clear it within the bound.
        constexpr size_t large_region = size * 4096;
        host.foreign_ranges.push_back({.address = naive_base, .size = large_region});

        const uint64_t host_base = mm.find_free_host_allocation_base(size, start);
        ASSERT_NE(host_base, 0u);
        ASSERT_GE(host_base, naive_base + large_region);
        ASSERT_TRUE(mm.host_window_is_free(host_base, size));
    }
} // namespace sogen::test
