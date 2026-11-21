#include <gtest/gtest.h>

#include <guest_page_slabs.hpp>

#include <vector>

namespace sogen::whp
{
    namespace
    {
        using slabs = guest_page_slabs;

        constexpr uint64_t window_base = 0x4000000000ull;
        constexpr uint64_t window_limit = window_base + (4 * slabs::slab_size);

        struct recorded_call
        {
            bool map{};
            uint64_t guest_physical_address{};
            size_t size{};
            bool host_committed{};
        };

        bool is_committed(const void* host)
        {
            MEMORY_BASIC_INFORMATION info{};
            return ::VirtualQuery(host, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT;
        }

        class GuestPageSlabsTest : public testing::Test
        {
          protected:
            std::vector<recorded_call> calls{};
            std::vector<std::pair<uint64_t, uint8_t*>> mapped_hosts{};
            uint64_t acknowledged_epoch = 0;

            slabs make(const slabs::budget limits = {.trim_above = ~size_t{0}, .trim_to = ~size_t{0}})
            {
                return slabs{window_base, window_limit,
                             slabs::mapper{
                                 .map_read_write =
                                     [this](const uint64_t gpa, void* host, const size_t size) {
                                         this->calls.push_back({.map = true, .guest_physical_address = gpa, .size = size});
                                         this->mapped_hosts.emplace_back(gpa, static_cast<uint8_t*>(host));
                                     },
                                 .unmap =
                                     [this](const uint64_t gpa, const size_t size) {
                                         this->calls.push_back({
                                             .guest_physical_address = gpa,
                                             .size = size,
                                             .host_committed = is_committed(this->host_of(gpa)),
                                         });
                                     },
                             },
                             limits};
            }

            slabs::epoch_acknowledged acknowledged() const
            {
                return [this](const uint64_t epoch) { return epoch <= this->acknowledged_epoch; };
            }

            const uint8_t* host_of(const uint64_t gpa) const
            {
                for (const auto& [chunk_gpa, host] : this->mapped_hosts)
                {
                    if (gpa >= chunk_gpa && gpa < chunk_gpa + slabs::chunk_size)
                    {
                        return host + (gpa - chunk_gpa);
                    }
                }
                return nullptr;
            }

            size_t count_calls(const bool map) const
            {
                return static_cast<size_t>(std::ranges::count_if(this->calls, [&](const auto& call) { return call.map == map; }));
            }
        };
    }

    // The hypervisor merges adjacent mappings only when guest-physical and host addresses both advance together.
    TEST_F(GuestPageSlabsTest, ConsecutivePagesAreContiguousInBothAddressSpaces)
    {
        auto memory = this->make();
        const auto first = memory.allocate(this->acknowledged());
        auto previous = first;
        for (size_t i = 1; i < slabs::pages_per_chunk + 8; ++i)
        {
            const auto page = memory.allocate(this->acknowledged());
            EXPECT_EQ(previous.guest_physical_address + slabs::page_size, page.guest_physical_address);
            EXPECT_EQ(previous.host + slabs::page_size, page.host);
            EXPECT_TRUE(page.mapped_read_write);
            previous = page;
        }

        EXPECT_EQ(window_base, first.guest_physical_address);
        ASSERT_EQ(2u, this->calls.size());
        EXPECT_EQ(slabs::chunk_size, this->calls[0].size);
        EXPECT_EQ(window_base + slabs::chunk_size, this->calls[1].guest_physical_address);
    }

    TEST_F(GuestPageSlabsTest, AFreedPageIsReusedOnlyAfterItsEpochAndComesBackZeroed)
    {
        auto memory = this->make();
        const auto page = memory.allocate(this->acknowledged());
        page.host[0] = 0x5a;
        memory.release(page.guest_physical_address, true, 7);

        this->acknowledged_epoch = 6;
        EXPECT_NE(page.guest_physical_address, memory.allocate(this->acknowledged()).guest_physical_address);

        this->acknowledged_epoch = 7;
        const auto reused = memory.allocate(this->acknowledged());
        EXPECT_EQ(page.guest_physical_address, reused.guest_physical_address);
        EXPECT_TRUE(reused.mapped_read_write);
        EXPECT_EQ(0, reused.host[0]);
    }

    TEST_F(GuestPageSlabsTest, APageReleasedWithoutItsMappingIsHandedOutUnmapped)
    {
        auto memory = this->make();
        const auto page = memory.allocate(this->acknowledged());
        memory.release(page.guest_physical_address, false, 0);
        EXPECT_FALSE(memory.allocate(this->acknowledged()).mapped_read_write);
    }

    TEST_F(GuestPageSlabsTest, TrimUnmapsFullyFreeChunksBeforeDecommittingThem)
    {
        auto memory = this->make({.trim_above = 0, .trim_to = 0});
        std::vector<slabs::page> pages{};
        pages.reserve(2 * slabs::pages_per_chunk);
        for (size_t i = 0; i < 2 * slabs::pages_per_chunk; ++i)
        {
            pages.push_back(memory.allocate(this->acknowledged()));
        }
        for (const auto& page : pages)
        {
            memory.release(page.guest_physical_address, true, 0);
        }

        memory.trim(this->acknowledged());

        ASSERT_EQ(2u, this->count_calls(false));
        for (const auto& call : this->calls)
        {
            if (!call.map)
            {
                EXPECT_EQ(slabs::chunk_size, call.size);
                EXPECT_TRUE(call.host_committed);
            }
        }
        EXPECT_FALSE(is_committed(pages.front().host));
        EXPECT_EQ(0u, memory.get_statistics().committed_bytes);
    }

    TEST_F(GuestPageSlabsTest, TrimKeepsPartiallyLiveAndUnacknowledgedChunks)
    {
        auto memory = this->make({.trim_above = 0, .trim_to = 0});
        std::vector<slabs::page> pages{};
        pages.reserve(2 * slabs::pages_per_chunk);
        for (size_t i = 0; i < 2 * slabs::pages_per_chunk; ++i)
        {
            pages.push_back(memory.allocate(this->acknowledged()));
        }

        for (size_t i = 0; i < pages.size(); ++i)
        {
            const bool straggler = i == 5;
            const bool unacknowledged = i >= slabs::pages_per_chunk;
            if (!straggler)
            {
                memory.release(pages[i].guest_physical_address, true, unacknowledged ? 1 : 0);
            }
        }

        memory.trim(this->acknowledged());

        EXPECT_EQ(0u, this->count_calls(false));
        const auto statistics = memory.get_statistics();
        EXPECT_EQ(slabs::page_size, statistics.live_bytes);
        EXPECT_EQ(2 * slabs::chunk_size, statistics.committed_bytes);
        EXPECT_EQ(slabs::chunk_size, statistics.fully_free_bytes);

        this->acknowledged_epoch = 1;
        memory.trim(this->acknowledged());
        EXPECT_EQ(1u, this->count_calls(false));
        EXPECT_EQ(window_base + slabs::chunk_size, this->calls.back().guest_physical_address);
    }

    TEST_F(GuestPageSlabsTest, TrimOnlyUnmapsTheMappedRunsOfAChunk)
    {
        auto memory = this->make({.trim_above = 0, .trim_to = 0});
        std::vector<slabs::page> pages{};
        pages.reserve(slabs::pages_per_chunk);
        for (size_t i = 0; i < slabs::pages_per_chunk; ++i)
        {
            pages.push_back(memory.allocate(this->acknowledged()));
        }
        for (size_t i = 0; i < pages.size(); ++i)
        {
            memory.release(pages[i].guest_physical_address, i != 10, 0);
        }

        memory.trim(this->acknowledged());

        ASSERT_EQ(2u, this->count_calls(false));
        EXPECT_EQ(10 * slabs::page_size, this->calls[1].size);
        EXPECT_EQ(window_base + (11 * slabs::page_size), this->calls[2].guest_physical_address);
        EXPECT_EQ((slabs::pages_per_chunk - 11) * slabs::page_size, this->calls[2].size);
    }

    TEST_F(GuestPageSlabsTest, TrimStopsAtTheBudget)
    {
        auto memory = this->make({.trim_above = slabs::chunk_size, .trim_to = slabs::chunk_size});
        std::vector<slabs::page> pages{};
        pages.reserve(3 * slabs::pages_per_chunk);
        for (size_t i = 0; i < 3 * slabs::pages_per_chunk; ++i)
        {
            pages.push_back(memory.allocate(this->acknowledged()));
        }
        for (const auto& page : pages)
        {
            memory.release(page.guest_physical_address, true, 0);
        }

        memory.trim(this->acknowledged());

        EXPECT_EQ(2u, this->count_calls(false));
        EXPECT_EQ(slabs::chunk_size, memory.get_statistics().fully_free_bytes);
    }

    TEST_F(GuestPageSlabsTest, TheGuestPageOfAnAddressFollowsAssignmentAndRelease)
    {
        auto memory = this->make();
        const auto page = memory.allocate(this->acknowledged());
        memory.guest_page(page.guest_physical_address) = 0x7ff600010000ull;
        EXPECT_EQ(0x7ff600010000ull, memory.guest_page_of(page.guest_physical_address));

        memory.release(page.guest_physical_address, true, 0);
        EXPECT_FALSE(memory.guest_page_of(page.guest_physical_address).has_value());
        EXPECT_FALSE(memory.guest_page_of(window_base - slabs::page_size).has_value());
    }
}
