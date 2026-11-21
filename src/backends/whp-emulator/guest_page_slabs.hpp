#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <vector>

namespace sogen::whp
{
    // Backs guest pages with large host reservations that each own a fixed guest-physical window, so a page's host
    // address and guest-physical address never change relative to each other. The hypervisor merges adjacent
    // mappings of contiguous host memory with equal flags into one range, and its map/unmap cost grows with the
    // number of ranges, so keeping freed pages mapped at a stable address keeps the partition at a few ranges per
    // slab instead of one per allocation.
    class guest_page_slabs
    {
      public:
        static constexpr size_t page_size = 0x1000;
        static constexpr size_t chunk_size = 2 * 1024 * 1024;
        static constexpr size_t slab_size = 64 * 1024 * 1024;
        static constexpr size_t pages_per_chunk = chunk_size / page_size;
        static constexpr size_t chunks_per_slab = slab_size / chunk_size;
        static constexpr size_t pages_per_slab = slab_size / page_size;

        struct budget
        {
            size_t trim_above{};
            size_t trim_to{};
        };

        struct mapper
        {
            std::function<void(uint64_t guest_physical_address, void* host, size_t size)> map_read_write;
            std::function<void(uint64_t guest_physical_address, size_t size)> unmap;
        };

        struct page
        {
            uint64_t guest_physical_address{};
            uint8_t* host{};
            bool mapped_read_write{};
        };

        struct statistics
        {
            size_t live_bytes{};
            size_t committed_bytes{};
            size_t fully_free_bytes{};
        };

        using epoch_acknowledged = std::function<bool(uint64_t epoch)>;

        guest_page_slabs(const uint64_t window_base, const uint64_t window_limit, mapper mapping, const budget limits)
            : window_base_(window_base),
              window_limit_(window_limit),
              mapping_(std::move(mapping)),
              budget_(limits)
        {
        }

        guest_page_slabs(const guest_page_slabs&) = delete;
        guest_page_slabs& operator=(const guest_page_slabs&) = delete;

        bool contains(const uint64_t guest_physical_address) const
        {
            return guest_physical_address >= this->window_base_ &&
                   guest_physical_address < this->window_base_ + (this->slabs_.size() * slab_size);
        }

        // Hands out the lowest reusable page, so consecutive allocations get consecutive addresses.
        page allocate(const epoch_acknowledged& acknowledged)
        {
            this->release_acknowledged_pages(acknowledged);
            if (this->slabs_with_free_pages_.empty())
            {
                this->commit_chunk();
            }

            const auto slab_index = *this->slabs_with_free_pages_.begin();
            auto& slab = *this->slabs_[slab_index];
            const auto first_free = slab.free_pages.begin();
            const auto page_index = *first_free;
            slab.free_pages.erase(first_free);
            if (slab.free_pages.empty())
            {
                this->slabs_with_free_pages_.erase(slab_index);
            }

            auto& state = slab.pages[page_index];
            state.live = true;
            this->add_live_page(slab.chunks[page_index / pages_per_chunk]);

            auto* host = slab.host + (page_index * page_size);
            if (state.dirty)
            {
                std::memset(host, 0, page_size);
                state.dirty = false;
            }

            return page{
                .guest_physical_address = slab.guest_physical_address + (page_index * page_size),
                .host = host,
                .mapped_read_write = state.mapped_read_write,
            };
        }

        // The page may only be handed out again once every vCPU acknowledged `epoch`. A page released without its
        // mapping must already be unmapped by the caller.
        void release(const uint64_t guest_physical_address, const bool still_mapped_read_write, const uint64_t epoch)
        {
            auto [slab, page_index] = this->locate(guest_physical_address);
            auto& state = slab.pages[page_index];
            if (!state.live)
            {
                throw std::runtime_error("WHP slab page released twice");
            }

            state.live = false;
            state.dirty = true;
            state.mapped_read_write = still_mapped_read_write;
            slab.guest_pages[page_index] = unassigned_guest_page;

            auto& chunk = slab.chunks[page_index / pages_per_chunk];
            ++chunk.pending_pages;
            this->remove_live_page(chunk);
            this->pending_.push_back(pending_page{.slab = slab.index, .page = page_index, .epoch = epoch});
        }

        // Decommits fully free chunks while more idle memory than the budget allows is committed.
        void trim(const epoch_acknowledged& acknowledged)
        {
            if (this->fully_free_chunks_ * chunk_size <= this->budget_.trim_above)
            {
                return;
            }

            this->release_acknowledged_pages(acknowledged);
            for (const auto& owned_slab : std::views::reverse(this->slabs_))
            {
                auto& slab = *owned_slab;
                for (size_t chunk_index = chunks_per_slab; chunk_index > 0; --chunk_index)
                {
                    if (this->fully_free_chunks_ * chunk_size <= this->budget_.trim_to)
                    {
                        return;
                    }

                    const auto& chunk = slab.chunks[chunk_index - 1];
                    if (chunk.committed && chunk.live_pages == 0 && chunk.pending_pages == 0)
                    {
                        this->decommit_chunk(slab, chunk_index - 1);
                    }
                }
            }
        }

        uint64_t& guest_page(const uint64_t guest_physical_address)
        {
            auto [slab, page_index] = this->locate(guest_physical_address);
            return slab.guest_pages[page_index];
        }

        std::optional<uint64_t> guest_page_of(const uint64_t guest_physical_address) const
        {
            if (!this->contains(guest_physical_address))
            {
                return std::nullopt;
            }

            const auto offset = guest_physical_address - this->window_base_;
            const auto& slab = *this->slabs_[offset / slab_size];
            const auto guest_page = slab.guest_pages[(offset % slab_size) / page_size];
            if (guest_page == unassigned_guest_page)
            {
                return std::nullopt;
            }

            return guest_page;
        }

        statistics get_statistics() const
        {
            return statistics{
                .live_bytes = this->live_pages_ * page_size,
                .committed_bytes = this->committed_chunks_ * chunk_size,
                .fully_free_bytes = this->fully_free_chunks_ * chunk_size,
            };
        }

        static constexpr uint64_t unassigned_guest_page = ~0ull;

      private:
        struct page_state
        {
            bool live{};
            bool dirty{};
            bool mapped_read_write{};
        };

        struct chunk_state
        {
            bool committed{};
            uint32_t live_pages{};
            uint32_t pending_pages{};
        };

        struct reservation_deleter
        {
            void operator()(uint8_t* memory) const
            {
                ::VirtualFree(memory, 0, MEM_RELEASE);
            }
        };

        struct slab
        {
            size_t index{};
            uint64_t guest_physical_address{};
            std::unique_ptr<uint8_t, reservation_deleter> reservation{};
            uint8_t* host{};
            std::array<chunk_state, chunks_per_slab> chunks{};
            std::vector<page_state> pages = std::vector<page_state>(pages_per_slab);
            std::vector<uint64_t> guest_pages = std::vector<uint64_t>(pages_per_slab, unassigned_guest_page);
            std::set<size_t> free_pages{};
        };

        struct pending_page
        {
            size_t slab{};
            size_t page{};
            uint64_t epoch{};
        };

        std::pair<slab&, size_t> locate(const uint64_t guest_physical_address)
        {
            if (!this->contains(guest_physical_address) || (guest_physical_address % page_size) != 0)
            {
                throw std::runtime_error("Address outside the WHP guest page slabs");
            }

            const auto offset = guest_physical_address - this->window_base_;
            return {*this->slabs_[offset / slab_size], static_cast<size_t>((offset % slab_size) / page_size)};
        }

        void release_acknowledged_pages(const epoch_acknowledged& acknowledged)
        {
            while (!this->pending_.empty() && acknowledged(this->pending_.front().epoch))
            {
                const auto pending = this->pending_.front();
                this->pending_.pop_front();

                auto& slab = *this->slabs_[pending.slab];
                --slab.chunks[pending.page / pages_per_chunk].pending_pages;
                slab.free_pages.insert(pending.page);
                this->slabs_with_free_pages_.insert(pending.slab);
            }
        }

        void commit_chunk()
        {
            for (auto& slab : this->slabs_)
            {
                for (size_t chunk_index = 0; chunk_index < chunks_per_slab; ++chunk_index)
                {
                    if (!slab->chunks[chunk_index].committed)
                    {
                        this->commit_chunk(*slab, chunk_index);
                        return;
                    }
                }
            }

            this->commit_chunk(this->reserve_slab(), 0);
        }

        slab& reserve_slab()
        {
            const auto guest_physical_address = this->window_base_ + (this->slabs_.size() * slab_size);
            if (guest_physical_address + slab_size > this->window_limit_)
            {
                throw std::runtime_error("WHP guest physical address space exhausted");
            }

            auto* host = static_cast<uint8_t*>(::VirtualAlloc(nullptr, slab_size, MEM_RESERVE, PAGE_READWRITE));
            if (host == nullptr)
            {
                throw std::runtime_error("VirtualAlloc failed while reserving guest memory");
            }

            auto new_slab = std::make_unique<slab>();
            new_slab->index = this->slabs_.size();
            new_slab->guest_physical_address = guest_physical_address;
            new_slab->reservation.reset(host);
            new_slab->host = host;
            this->slabs_.push_back(std::move(new_slab));
            return *this->slabs_.back();
        }

        void commit_chunk(slab& slab, const size_t chunk_index)
        {
            auto* host = slab.host + (chunk_index * chunk_size);
            if (::VirtualAlloc(host, chunk_size, MEM_COMMIT, PAGE_READWRITE) == nullptr)
            {
                throw std::runtime_error("VirtualAlloc failed while committing guest memory");
            }

            this->mapping_.map_read_write(slab.guest_physical_address + (chunk_index * chunk_size), host, chunk_size);

            slab.chunks[chunk_index].committed = true;
            ++this->committed_chunks_;
            ++this->fully_free_chunks_;

            const auto first_page = chunk_index * pages_per_chunk;
            for (size_t page_index = first_page; page_index < first_page + pages_per_chunk; ++page_index)
            {
                slab.pages[page_index] = page_state{.mapped_read_write = true};
                slab.free_pages.insert(page_index);
            }
            this->slabs_with_free_pages_.insert(slab.index);
        }

        void decommit_chunk(slab& slab, const size_t chunk_index)
        {
            const auto first_page = chunk_index * pages_per_chunk;
            const auto last_page = first_page + pages_per_chunk;
            for (size_t page_index = first_page; page_index < last_page;)
            {
                if (!slab.pages[page_index].mapped_read_write)
                {
                    ++page_index;
                    continue;
                }

                auto run_end = page_index + 1;
                while (run_end < last_page && slab.pages[run_end].mapped_read_write)
                {
                    ++run_end;
                }

                this->mapping_.unmap(slab.guest_physical_address + (page_index * page_size), (run_end - page_index) * page_size);
                page_index = run_end;
            }

            // The hypervisor mapping must be gone before the host memory behind it is decommitted.
            if (!::VirtualFree(slab.host + (chunk_index * chunk_size), chunk_size, MEM_DECOMMIT))
            {
                throw std::runtime_error("VirtualFree failed while decommitting guest memory");
            }

            for (size_t page_index = first_page; page_index < last_page; ++page_index)
            {
                slab.pages[page_index] = {};
                slab.free_pages.erase(page_index);
            }
            if (slab.free_pages.empty())
            {
                this->slabs_with_free_pages_.erase(slab.index);
            }

            slab.chunks[chunk_index].committed = false;
            --this->committed_chunks_;
            --this->fully_free_chunks_;
        }

        void add_live_page(chunk_state& chunk)
        {
            if (chunk.live_pages++ == 0)
            {
                --this->fully_free_chunks_;
            }
            ++this->live_pages_;
        }

        void remove_live_page(chunk_state& chunk)
        {
            if (--chunk.live_pages == 0)
            {
                ++this->fully_free_chunks_;
            }
            --this->live_pages_;
        }

        uint64_t window_base_{};
        uint64_t window_limit_{};
        mapper mapping_{};
        budget budget_{};
        std::vector<std::unique_ptr<slab>> slabs_{};
        std::set<size_t> slabs_with_free_pages_{};
        std::deque<pending_page> pending_{};
        size_t live_pages_{};
        size_t committed_chunks_{};
        size_t fully_free_chunks_{};
    };
}
