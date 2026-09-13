#pragma once

#include "../std_include.hpp"

#include "../host_range_reader.hpp"
#include "../macos_memory_manager.hpp"
#include "dyld_shared_cache.hpp"

#include <functional>
#include <list>
#include <span>
#include <unordered_map>
#include <vector>

#include <memory_permission.hpp>

namespace sogen
{
    constexpr uint64_t MACOS_CACHE_CHUNK_SIZE = 0x200000ULL;
    constexpr uint64_t MACOS_CACHE_RESIDENCY_BUDGET_DEFAULT = 512ULL * 1024ULL * 1024ULL;

    struct dyld_cache_backing_range
    {
        uint64_t address{};
        uint64_t size{};
        std::filesystem::path path{};
        uint64_t file_offset{};
        memory_permission permissions{};
    };

    std::vector<dyld_cache_backing_range> build_dyld_cache_backing_ranges(const dyld_shared_cache_reader& cache, uint64_t slide);

    using dyld_cache_chunk_fixup =
        std::function<void(uint64_t chunk_address, std::span<std::byte> chunk_data, memory_permission permissions)>;

    // Materialises the shared cache a chunk at a time as the guest touches it, keeping residency under a
    // budget. The whole cache is 5.4 GB; a browser cannot hold it and a native run has no reason to.
    class dyld_cache_pager
    {
      public:
        dyld_cache_pager(macos_memory_manager& memory, host_range_reader& reader, std::vector<dyld_cache_backing_range> ranges,
                         uint64_t residency_budget = MACOS_CACHE_RESIDENCY_BUDGET_DEFAULT);

        dyld_cache_pager(const dyld_cache_pager&) = delete;
        dyld_cache_pager& operator=(const dyld_cache_pager&) = delete;
        dyld_cache_pager(dyld_cache_pager&&) = delete;
        dyld_cache_pager& operator=(dyld_cache_pager&&) = delete;
        ~dyld_cache_pager() = default;

        void set_chunk_fixup(dyld_cache_chunk_fixup fixup);

        bool covers(uint64_t address) const;
        bool page_in(uint64_t address);

        // Keeps a resident chunk out of the LRU's reach. A host-side patch (the GUI's svc traps) lives
        // only in the mapped bytes: an evicted chunk is re-fetched from the file, and the file has never
        // heard of the patch.
        bool pin(uint64_t address);

        // Reads from the backing file without materialising anything, for data that must be readable
        // while a fault is being handled -- the slide blob above all, which lives in the cache's
        // read-only region and would otherwise fault the pager re-entrantly.
        bool read_backing(uint64_t address, std::span<std::byte> destination) const;

        uint64_t span_start() const
        {
            return this->span_start_;
        }

        uint64_t span_end() const
        {
            return this->span_end_;
        }

        uint64_t resident_bytes() const
        {
            return this->resident_bytes_;
        }

        uint64_t resident_chunks() const
        {
            return this->chunks_.size();
        }

        uint64_t paged_in_chunks() const
        {
            return this->paged_in_chunks_;
        }

        uint64_t evicted_chunks() const
        {
            return this->evicted_chunks_;
        }

        uint64_t bytes_read() const
        {
            return this->bytes_read_;
        }

      private:
        struct resident_chunk
        {
            uint64_t address{};
            uint64_t size{};
            bool pinned{};
        };

        const dyld_cache_backing_range* find_range(uint64_t address) const;
        void evict_until_fits(uint64_t needed);

        macos_memory_manager* memory_{};
        host_range_reader* reader_{};
        std::vector<dyld_cache_backing_range> ranges_{};
        dyld_cache_chunk_fixup fixup_{};
        uint64_t span_start_{};
        uint64_t span_end_{};
        uint64_t residency_budget_{};
        uint64_t resident_bytes_{};
        uint64_t paged_in_chunks_{};
        uint64_t evicted_chunks_{};
        uint64_t bytes_read_{};
        std::list<resident_chunk> lru_{};

        // Keyed on the address actually mapped, not on the chunk-aligned address. In the real cache 33 of
        // 34 regions start off a 2 MiB boundary and 27 pairs share a chunk, sometimes with different
        // permissions; keying on the aligned address would make the second region of such a pair look
        // resident because the first was, and it would never be mapped at all.
        std::unordered_map<uint64_t, std::list<resident_chunk>::iterator> chunks_{};
    };

    class macos_emulator;

    void install_dyld_cache_pager(macos_emulator& emu, std::unique_ptr<dyld_cache_pager> pager);
}
