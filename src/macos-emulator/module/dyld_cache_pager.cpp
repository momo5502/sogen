#include "../std_include.hpp"
#include "dyld_cache_pager.hpp"

#include "../macos_emulator.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>

namespace sogen
{
    namespace
    {
        bool range_is_representable(const uint64_t address, const uint64_t size)
        {
            return size != 0 && size <= std::numeric_limits<uint64_t>::max() - address;
        }
    }

    std::vector<dyld_cache_backing_range> build_dyld_cache_backing_ranges(const dyld_shared_cache_reader& cache, const uint64_t slide)
    {
        std::vector<dyld_cache_backing_range> ranges{};

        for (const auto& file : cache.files())
        {
            for (const auto& region : file.regions)
            {
                if (slide > std::numeric_limits<uint64_t>::max() - region.address)
                {
                    continue;
                }

                const auto address = region.address + slide;
                if (!range_is_representable(address, region.size))
                {
                    continue;
                }

                ranges.push_back(dyld_cache_backing_range{
                    .address = address,
                    .size = region.size,
                    .path = file.path,
                    .file_offset = region.file_offset,
                    .permissions = region.initial_permissions,
                });
            }
        }

        std::ranges::sort(ranges, {}, &dyld_cache_backing_range::address);
        return ranges;
    }

    dyld_cache_pager::dyld_cache_pager(macos_memory_manager& memory, host_range_reader& reader,
                                       std::vector<dyld_cache_backing_range> ranges, const uint64_t residency_budget)
        : memory_(&memory),
          reader_(&reader),
          residency_budget_(std::max<uint64_t>(residency_budget, MACOS_CACHE_CHUNK_SIZE))
    {
        for (auto& range : ranges)
        {
            if (!range_is_representable(range.address, range.size))
            {
                continue;
            }

            const auto end = range.address + range.size;

            if (this->ranges_.empty())
            {
                this->span_start_ = range.address;
                this->span_end_ = end;
            }
            else
            {
                this->span_start_ = std::min(this->span_start_, range.address);
                this->span_end_ = std::max(this->span_end_, end);
            }

            this->ranges_.push_back(std::move(range));
        }

        // Sorted here rather than relying on the caller: find_range binary searches, and it runs on every
        // fault into a 5.4 GB span.
        std::ranges::sort(this->ranges_, {}, &dyld_cache_backing_range::address);
    }

    void dyld_cache_pager::set_chunk_fixup(dyld_cache_chunk_fixup fixup)
    {
        this->fixup_ = std::move(fixup);
    }

    const dyld_cache_backing_range* dyld_cache_pager::find_range(const uint64_t address) const
    {
        const auto next = std::ranges::upper_bound(this->ranges_, address, {}, &dyld_cache_backing_range::address);
        if (next == this->ranges_.begin())
        {
            return nullptr;
        }

        const auto& candidate = *std::prev(next);
        if ((address - candidate.address) < candidate.size)
        {
            return &candidate;
        }

        return nullptr;
    }

    bool dyld_cache_pager::covers(const uint64_t address) const
    {
        return this->find_range(address) != nullptr;
    }

    bool dyld_cache_pager::read_backing(const uint64_t address, const std::span<std::byte> destination) const
    {
        if (destination.empty())
        {
            return true;
        }

        auto remaining = destination;
        auto cursor = address;

        // A read can straddle two ranges of the same file, and the ranges are not contiguous in the file
        // even where they are contiguous in memory, so each piece is resolved separately.
        while (!remaining.empty())
        {
            const auto* range = this->find_range(cursor);
            if (range == nullptr)
            {
                return false;
            }

            const auto offset_in_range = cursor - range->address;
            const auto available = std::min<uint64_t>(remaining.size(), range->size - offset_in_range);
            const auto piece = remaining.first(static_cast<size_t>(available));

            if (this->reader_->read(range->path.string(), range->file_offset + offset_in_range, piece) != piece.size())
            {
                return false;
            }

            cursor += available;
            remaining = remaining.subspan(static_cast<size_t>(available));
        }

        return true;
    }

    bool dyld_cache_pager::pin(const uint64_t address)
    {
        const auto* range = this->find_range(address);
        if (range == nullptr)
        {
            return false;
        }

        const auto chunk_begin = std::max(address & ~(MACOS_CACHE_CHUNK_SIZE - 1), range->address);
        const auto found = this->chunks_.find(chunk_begin);
        if (found == this->chunks_.end())
        {
            return false;
        }

        found->second->pinned = true;
        return true;
    }

    void dyld_cache_pager::evict_until_fits(const uint64_t needed)
    {
        if (needed > this->residency_budget_)
        {
            return;
        }

        const auto limit = this->residency_budget_ - needed;

        while (this->resident_bytes_ > limit)
        {
            // From the back, so the first unpinned entry found is the least recently used one.
            const auto victim =
                std::find_if(this->lru_.rbegin(), this->lru_.rend(), [](const resident_chunk& chunk) { return !chunk.pinned; });

            if (victim == this->lru_.rend())
            {
                return;
            }

            const auto forward = std::next(victim).base();

            if (!this->memory_->release_memory(forward->address, static_cast<size_t>(forward->size)))
            {
                return;
            }

            this->resident_bytes_ -= forward->size;
            ++this->evicted_chunks_;
            this->chunks_.erase(forward->address);
            this->lru_.erase(forward);
        }
    }

    bool dyld_cache_pager::page_in(const uint64_t address)
    {
        const auto* range = this->find_range(address);
        if (range == nullptr)
        {
            return false;
        }

        const auto chunk_start = address & ~(MACOS_CACHE_CHUNK_SIZE - 1);
        const auto range_end = range->address + range->size;

        const auto chunk_begin = std::max(chunk_start, range->address);
        const auto chunk_end = std::min(chunk_start + MACOS_CACHE_CHUNK_SIZE, range_end);

        if (chunk_end <= chunk_begin)
        {
            return false;
        }

        const auto existing = this->chunks_.find(chunk_begin);
        if (existing != this->chunks_.end())
        {
            this->lru_.splice(this->lru_.begin(), this->lru_, existing->second);
            return true;
        }

        const auto chunk_size = chunk_end - chunk_begin;

        std::vector<std::byte> data(static_cast<size_t>(chunk_size));
        const auto file_offset = range->file_offset + (chunk_begin - range->address);
        this->bytes_read_ += this->reader_->read(range->path.string(), file_offset, data);

        if (this->fixup_)
        {
            this->fixup_(chunk_begin, data, range->permissions);
        }

        this->evict_until_fits(chunk_size);

        if (!this->memory_->allocate_memory(chunk_begin, static_cast<size_t>(chunk_size), range->permissions))
        {
            return false;
        }

        if (!this->memory_->try_write_memory(chunk_begin, data.data(), data.size()))
        {
            (void)this->memory_->release_memory(chunk_begin, static_cast<size_t>(chunk_size));
            return false;
        }

        // Cache DATA that the guest writes to exists nowhere else: the backing file still holds the
        // original bytes, so evicting a writable chunk would discard the write and read back the old
        // value. Pinned rather than written back, because the file is the host's and must not be touched.
        const auto pinned = (range->permissions & memory_permission::write) != memory_permission::none;

        this->lru_.push_front(resident_chunk{.address = chunk_begin, .size = chunk_size, .pinned = pinned});
        this->chunks_.emplace(chunk_begin, this->lru_.begin());

        this->resident_bytes_ += chunk_size;
        ++this->paged_in_chunks_;

        return true;
    }

    namespace
    {
        std::string format_address(const uint64_t value)
        {
            std::array<char, 19> buffer{};
            (void)std::snprintf(buffer.data(), buffer.size(), "0x%llx", static_cast<unsigned long long>(value));
            return buffer.data();
        }
    }

    void install_dyld_cache_pager(macos_emulator& emu, std::unique_ptr<dyld_cache_pager> pager)
    {
        auto* raw = pager.get();
        emu.cache_pager = std::move(pager);

        // Walked a page at a time rather than a chunk at a time: a chunk can be shared by two regions,
        // and each half is a separate entry that the other's address does not materialise.
        emu.memory.set_demand_pager([raw](const uint64_t address, const size_t size) {
            if (!range_is_representable(address, size) || raw->span_end() <= raw->span_start())
            {
                return false;
            }

            // Clamped to the cache's span before the walk: almost every failed read is nowhere near it,
            // and a guest-supplied length must not turn one into a walk over millions of pages.
            const auto first = std::max(address, raw->span_start());
            const auto last = std::min(address + size - 1, raw->span_end() - 1);

            bool materialized = false;

            for (auto cursor = first; cursor <= last; cursor = (cursor | (MACOS_PAGE_SIZE - 1)) + 1)
            {
                materialized |= raw->covers(cursor) && raw->page_in(cursor);
            }

            return materialized;
        });

        // Unicorn calls every registered invalid-memory hook and takes the access as handled if any one
        // of them says so, which is what lets this sit alongside the catch-all fault hook the emulator
        // already installs.
        (void)emu.emu().hook_memory_violation(
            [raw, &emu](cpu_interface&, const uint64_t address, size_t, memory_operation, const memory_violation_type type) {
                if (type != memory_violation_type::unmapped || !raw->covers(address))
                {
                    return memory_violation_continuation::stop;
                }

                // Reported rather than resumed: telling the backend the fault is handled when nothing was
                // mapped puts the guest in a loop re-faulting on the same address, which looks like a hang
                // rather than the read failure it is.
                if (!raw->page_in(address))
                {
                    emu.record_stop(stop_reason::unhandled_memory_violation, "dyld cache page-in failed at " + format_address(address));
                    return memory_violation_continuation::stop;
                }

                return memory_violation_continuation::resume;
            });
    }
}
