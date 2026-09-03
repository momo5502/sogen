#pragma once

#include "std_include.hpp"
#include "host_file_mapping.hpp"

#include "macos_platform.hpp"

#include <address_utils.hpp>
#include <memory_interface.hpp>
#include <memory_permission.hpp>
#include <memory_region.hpp>
#include <serialization.hpp>

#include <functional>

namespace sogen
{
    struct macos_memory_region
    {
        size_t length{};
        memory_permission permissions{};

        // XNU's max_protection. It is what separates the two kinds of unbacked range: libmalloc's
        // PROT_NONE magazine reservation keeps the default of everything and becomes real memory the
        // first time the guest protects a slice, while __PAGEZERO is mapped with max_protection NONE so
        // a guest can never protect its way into dereferencing null.
        memory_permission max_permissions{memory_permission::all};
        bool backed{};
        bool file_backed{};
        std::string backing_path{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    struct macos_memory_region_info : basic_memory_region<memory_permission>
    {
        uint64_t allocation_base{};
        size_t allocation_length{};
        memory_permission initial_permissions{};
        bool is_file_backed{};
        std::string backing_path{};
    };

    // Materialises whatever part of the given range is served on demand, and reports whether anything
    // became mapped that was not before.
    using macos_demand_pager = std::function<bool(uint64_t address, size_t size)>;

    class macos_memory_manager : public memory_interface
    {
      public:
        using region_map = std::map<uint64_t, macos_memory_region>;

        explicit macos_memory_manager(memory_interface& memory)
            : memory_(&memory)
        {
        }

        // Only a guest access faults, so a host-side read of the on-demand shared cache -- the GUI
        // interception reading a class name, a CFString, the first word of a guest-call target -- sees an
        // evicted chunk as absent and silently drops whatever it was inspecting. Retrying the read
        // through this is what makes the mapping look the same from the host side as from the guest's.
        void set_demand_pager(macos_demand_pager pager);

        void read_memory(uint64_t address, void* data, size_t size) const override;
        bool try_read_memory(uint64_t address, void* data, size_t size) const override;
        void write_memory(uint64_t address, const void* data, size_t size) override;
        bool try_write_memory(uint64_t address, const void* data, size_t size) override;

        bool allocate_memory(uint64_t address, size_t size, memory_permission permissions);
        uint64_t allocate_memory(size_t size, memory_permission permissions, uint64_t start = 0);

        // Claims a range without giving it any backing store. __PAGEZERO needs this: mapping its 4 GiB
        // through map_memory would commit 4 GiB of host RAM and, worse, turn a guest null dereference
        // into a successful access.
        bool reserve_memory(uint64_t address, size_t size);

        // The commpage lives above MACOS_MAX_MMAP_END_EXCL inside the nesting region that
        // is_reserved_range keeps every other caller out of. This is the only door into it.
        bool allocate_reserved_region(uint64_t address, size_t size, memory_permission permissions);

        bool protect_memory(uint64_t address, size_t size, memory_permission permissions);

        // What MADV_ZERO does. Whole pages only, because madvise rounds the range out to page bounds
        // before the map ever sees it, and false for a range that is not entirely mapped and writable --
        // the caller has to report that rather than leave the guest believing the range was cleared.
        bool zero_memory(uint64_t address, size_t size);

      private:
        bool materialize(uint64_t address, size_t size) const;
        void apply_permissions_to_region(uint64_t base, macos_memory_region& region, memory_permission permissions);
        void claim_for_emulator(uint64_t address, size_t length);

      public:
        bool release_memory(uint64_t address, size_t size);
        bool map_host_file_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions);

        // Page-aligned host memory this manager owns for the rest of the run, so a guest range can be
        // mapped at two addresses and really be the same bytes. unicorn never frees a uc_mem_map_ptr
        // block, so the owner has to outlive every mapping over it -- which is why the caller cannot
        // supply its own buffer. Null when the allocation fails.
        void* acquire_shared_backing(size_t size);

        // Reads the requested file range into private guest pages rather than aliasing the host mapping.
        // The copy is what makes a guest write to a file mapping unable to reach the user's real file;
        // Stage 5 replaces it for the dyld shared cache, whose 5.4 GiB span no copy can serve.
        bool map_file(uint64_t address, size_t size, memory_permission permissions, const std::filesystem::path& host_path,
                      uint64_t file_offset);

        bool map_host_file_range(uint64_t address, size_t size, const std::filesystem::path& host_path, uint64_t file_offset,
                                 memory_permission permissions);

        size_t get_host_mapping_count() const
        {
            return this->host_mappings_.size();
        }

        void unmap_all_memory();

        // avoid_emulator_ranges keeps the search off every address the emulator has claimed. It is a
        // parameter rather than an inference from `start` because a guest search can name one too:
        // mach_traps' aligned-base retry walks forward with a hint, and inferring from it handed the
        // guest emulator memory.
        uint64_t find_free_allocation_base(size_t size, uint64_t start = 0, bool avoid_emulator_ranges = true) const;
        bool overlaps_mapped_region(uint64_t address, size_t size) const;
        static bool is_reserved_range(uint64_t address, size_t size);

        const region_map& get_mapped_regions() const
        {
            return this->mapped_regions_;
        }

        std::vector<macos_memory_region_info> get_mapped_region_infos() const;
        std::optional<macos_memory_region_info> get_region_info(uint64_t address) const;

        uint64_t get_mmap_base() const
        {
            return this->mmap_base_;
        }

        void set_mmap_base(const uint64_t address)
        {
            this->mmap_base_ = address;
        }

        void serialize_memory_state(utils::buffer_serializer& buffer) const;
        void deserialize_memory_state(utils::buffer_deserializer& buffer);

      private:
        void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override;
        void map_memory(uint64_t address, size_t size, memory_permission permissions) override;
        void unmap_memory(uint64_t address, size_t size) override;
        void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override;

        bool claim_region(uint64_t address, size_t size, memory_permission permissions, uint64_t end_excl);
        static void validate_restored_regions(const region_map& regions);
        void apply_region_edits(const std::vector<uint64_t>& removals, std::vector<std::pair<uint64_t, macos_memory_region>> replacements);

        memory_interface* memory_{};
        region_map mapped_regions_{};
        std::vector<std::pair<uint64_t, host_file_mapping>> host_mappings_{};
        std::vector<std::unique_ptr<void, void (*)(void*)>> shared_backings_{};

        // Every range the emulator has ever taken for a buffer of its own -- a window backing store, a
        // layer's contents, the CoreFoundation bridge's scratch, an IOSurface. Kept for the lifetime of
        // the run rather than dropped on release: the guest is handed these addresses and sogen is never
        // told when it has finished with them.
        std::map<uint64_t, size_t> emulator_ranges_{};
        uint64_t mmap_base_{MACOS_DEFAULT_MMAP_BASE};

        macos_demand_pager demand_pager_{};

        // Paging a chunk in reads and writes guest memory itself. Without this a read that failed inside
        // that work would ask for the same chunk again, one level deeper.
        mutable bool demand_paging_{};
    };
}
