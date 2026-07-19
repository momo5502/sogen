#pragma once
#include "std_include.hpp"
#include <map>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

#include "memory_permission_ext.hpp"
#include "memory_region.hpp"
#include "serialization.hpp"

#include <memory_interface.hpp>

namespace sogen
{

    constexpr auto ALLOCATION_GRANULARITY = 0x0000000000010000ULL;
    constexpr auto MIN_ALLOCATION_ADDRESS = 0x0000000000010000ULL;
    constexpr auto MAX_ALLOCATION_ADDRESS = 0x00007ffffffeffffULL;
    constexpr auto MAX_ALLOCATION_END_EXCL = MAX_ALLOCATION_ADDRESS + 1ULL;
    constexpr auto DEFAULT_ALLOCATION_ADDRESS_64BIT = 0x100000000ULL;
    constexpr auto DEFAULT_ALLOCATION_ADDRESS_32BIT = 0x10000ULL;

    enum class memory_region_kind : uint8_t
    {
        free = 0,
        private_allocation,
        file_section_view,
        pagefile_section_view,
        section_image,
        mmio,
        host_reserved,
    };

    // This maps to the `basic_memory_region` struct defined in
    // emulator\memory_region.hpp
    struct region_info : basic_memory_region<nt_memory_permission>
    {
        uint64_t allocation_base{};
        size_t allocation_length{};
        bool is_reserved{};
        bool is_committed{};
        nt_memory_permission initial_permissions{};
        memory_region_kind kind{memory_region_kind::free};
    };

    using mmio_read_callback = std::function<void(uint64_t addr, void* data, size_t size)>;
    using mmio_write_callback = std::function<void(uint64_t addr, const void* data, size_t size)>;

    struct memory_stats
    {
        uint64_t reserved_memory = 0;
        uint64_t committed_memory = 0;
    };

    class memory_manager : public memory_interface
    {
      public:
        memory_manager(memory_interface& memory)
            : memory_(&memory)
        {
        }

        struct committed_region
        {
            size_t length{};
            nt_memory_permission permissions{};
        };

        using committed_region_map = std::map<uint64_t, committed_region>;

        struct reserved_region
        {
            size_t length{};
            memory_permission initial_permission{};
            committed_region_map committed_regions{};
            memory_region_kind kind{memory_region_kind::private_allocation};
            std::u16string mapped_filename{};
        };

        using reserved_region_map = std::map<uint64_t, reserved_region>;

        void read_memory(uint64_t address, void* data, size_t size) const final;
        bool try_read_memory(uint64_t address, void* data, size_t size) const final;
        void write_memory(uint64_t address, const void* data, size_t size) final;
        bool try_write_memory(uint64_t address, const void* data, size_t size) final;

        bool protect_memory(uint64_t address, size_t size, nt_memory_permission permissions,
                            nt_memory_permission* old_permissions = nullptr);

        // Asks the backend which of its own host address ranges the guest address space must avoid
        // (see memory_interface::reserved_host_ranges) and pre-reserves any newly-discovered ones so
        // future allocations steer clear. No-op for backends with an independent guest address space
        // (the default). Must be called before any guest memory is allocated in the range(s) it
        // reserves to be effective. Cheap and safe to call frequently (e.g. before every dynamic
        // allocation) - only ever adds ranges, never releases existing ones, so it can't momentarily
        // drop a reservation. See reset_host_memory_ranges for the rare case that needs to release
        // and re-query from scratch.
        void reserve_host_memory_ranges();

        // Windowed form of reserve_host_memory_ranges: reserves only backend host ranges intersecting
        // [address, size). Used by the fixed-address allocate_memory overload, which only needs its
        // own target window checked, not a full-address-space rescan on every module (re)map.
        void reserve_host_memory_ranges_in(uint64_t address, size_t size);

        // True if no foreign host mapping currently intersects [address, size), via a bounded,
        // windowed backend probe (usually a single query, unlike reserve_host_memory_ranges' full
        // address-space rescan). Records nothing - a pure check, for callers that only want to
        // confirm a candidate before committing to it (see the size-only allocate_memory overload
        // and handle_NtAllocateVirtualMemoryEx) rather than track the result.
        bool host_window_is_free(uint64_t address, size_t size) const;

        // Like reserve_host_memory_ranges, but first releases every previously-tracked range before
        // re-querying the backend - needed only when the backend's answer can genuinely change. Not
        // safe to call from a hot path: doubles the syscall count of a routine re-scan for no
        // benefit, and momentarily un-reserves everything, widening a real race window against
        // anything else in the process mapping host memory concurrently.
        void reset_host_memory_ranges();

        bool allocate_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb);
        // Reserves the range and aliases it onto caller-owned host memory (e.g. a host Vulkan mapping) so the
        // guest accesses it coherently. The region is treated like MMIO: not serialized, host_pointer not owned.
        bool allocate_host_memory(uint64_t address, size_t size, void* host_pointer, nt_memory_permission permissions);

        // Backend coherency hooks for host-aliased memory (see memory_interface). Device emulation such as
        // the GPU bridge uses these to make guest writes visible to the host GPU on backends (e.g. KVM) that
        // alias host memory into the guest non-coherently.
        bool host_memory_aliasing_is_coherent() const override;
        void flush_host_memory_cache(const void* host_pointer, size_t size) override;
        bool allocate_memory(uint64_t address, size_t size, nt_memory_permission permissions, bool reserve_only = false,
                             memory_region_kind kind = memory_region_kind::private_allocation);

        bool commit_memory(uint64_t address, size_t size, nt_memory_permission permissions);
        bool commit_image_memory(uint64_t address, size_t size, nt_memory_permission permissions);
        bool decommit_memory(uint64_t address, size_t size);

        bool release_memory(uint64_t address, size_t size);

        void unmap_all_memory();

        uint64_t allocate_memory(size_t size, nt_memory_permission permissions, bool reserve_only = false, uint64_t start = 0,
                                 memory_region_kind kind = memory_region_kind::private_allocation);

        uint64_t find_free_allocation_base(size_t size, uint64_t start = 0) const;
        uint64_t find_free_allocation_base(size_t size, uint64_t start, uint64_t alignment, uint64_t lowest_address,
                                           uint64_t highest_address) const;

        // Like find_free_allocation_base, but also confirms the pick is actually free at the host level
        // (not merely per sogen's own bookkeeping) and rescans + re-picks past any foreign host mapping
        // that has claimed it since the last scan, bounded by an internal retry cap (returns 0 if none
        // could be confirmed). On backends with an independent guest address space (the default) this is
        // identical to find_free_allocation_base - host_window_is_free is always true there. Shared by the
        // size-only allocate_memory overload and the fixed-address module-relocation fallback so both get
        // the same host-race recovery; see the size-only allocate_memory overload for the full rationale.
        uint64_t find_free_host_allocation_base(size_t size, uint64_t start);

        // Same as above, but bounds the search to [MIN_ALLOCATION_ADDRESS, highest_address] instead of the
        // full address space - for callers with a hard architectural ceiling (e.g. a below-4GB requirement)
        // where a pick above that ceiling would be useless regardless of whether it's free.
        uint64_t find_free_host_allocation_base(size_t size, uint64_t start, uint64_t highest_address);

        region_info get_region_info(uint64_t address);
        std::optional<std::u16string> get_region_mapped_filename(uint64_t address) const;
        void set_region_mapped_filename(uint64_t address, std::u16string filename);

        reserved_region_map::iterator find_reserved_region(uint64_t address);

        // ignore_host_reserved skips conflicts against memory_region_kind::host_reserved entries (see
        // reserve_host_memory_ranges) - used by allocate_mmio, since an MMIO region is trapped via
        // fault handling rather than backed by real host memory, so it doesn't need the backend's own
        // host address space to be free there.
        bool overlaps_reserved_region(uint64_t address, size_t size, bool ignore_host_reserved = false) const;

        memory_region_kind get_region_kind(uint64_t address) const;

        const reserved_region_map& get_reserved_regions() const
        {
            return this->reserved_regions_;
        }

        std::uint64_t get_layout_version() const
        {
            return this->layout_version_.load(std::memory_order_relaxed);
        }

        std::uint64_t get_default_allocation_address() const
        {
            return this->default_allocation_address_;
        }

        void set_default_allocation_address(std::uint64_t address)
        {
            this->default_allocation_address_ = address;
        }

        void serialize_memory_state(utils::buffer_serializer& buffer, bool is_snapshot) const;
        void deserialize_memory_state(utils::buffer_deserializer& buffer, bool is_snapshot);

        memory_stats compute_memory_stats() const;

        void set_dep_enabled(bool enabled);

        bool is_dep_enabled() const
        {
            return this->dep_enabled_;
        }

      private:
        memory_interface* memory_{};
        reserved_region_map reserved_regions_{};
        std::atomic<std::uint64_t> layout_version_{0};
        std::uint64_t default_allocation_address_{0x100000000ULL};
        bool dep_enabled_{true};
        // Addresses reserved by reserve_host_memory_ranges() so far, so reset_host_memory_ranges can
        // release the previous set before asking the backend for a fresh one.
        std::vector<uint64_t> host_reserved_addresses_{};

        void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) final;
        void map_memory(uint64_t address, size_t size, memory_permission permissions) final;
        void map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) final;
        void unmap_memory(uint64_t address, size_t size) final;
        void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) final;

        void update_layout_version();
        bool commit_memory(uint64_t address, size_t size, nt_memory_permission permissions, bool allow_image_section);
        memory_permission get_effective_permissions(nt_memory_permission permissions) const;

        // The actual fixed-address allocation logic, without the reserve_host_memory_ranges rescan
        // the public allocate_memory(address, ...) performs first. reserve_host_memory_ranges itself
        // calls this (not the public overload) for each backend-reported range, to avoid recursing
        // back into reserve_host_memory_ranges.
        bool allocate_memory_raw(uint64_t address, size_t size, nt_memory_permission permissions, bool reserve_only,
                                 memory_region_kind kind);

        void reserve_host_range_gaps(uint64_t address, size_t size);
        void carve_host_reserved_hole(uint64_t address, size_t size);
        void release_host_claims(uint64_t released_end);
    };

    namespace memory_region_policy
    {
        constexpr bool is_section_kind(const memory_region_kind kind)
        {
            return kind == memory_region_kind::pagefile_section_view || kind == memory_region_kind::file_section_view ||
                   kind == memory_region_kind::section_image;
        }

        constexpr bool is_mapped_memory_kind(const memory_region_kind kind)
        {
            return is_section_kind(kind) || kind == memory_region_kind::mmio;
        }

        constexpr uint32_t to_memory_basic_information_type(const memory_region_kind kind)
        {
            switch (kind)
            {
            case memory_region_kind::section_image:
                return MEM_IMAGE;
            case memory_region_kind::pagefile_section_view:
            case memory_region_kind::file_section_view:
                return MEM_MAPPED;
            case memory_region_kind::mmio:
                return MEM_MAPPED;
            default:
                return MEM_PRIVATE;
            }
        }

        constexpr uint32_t to_memory_region_information_type(const memory_region_kind kind)
        {
            switch (kind)
            {
            case memory_region_kind::private_allocation:
                return 1 << 0;
            case memory_region_kind::file_section_view:
                return 1 << 1;
            case memory_region_kind::section_image:
                return 1 << 2;
            case memory_region_kind::pagefile_section_view:
                return 1 << 3;
            case memory_region_kind::mmio:
                return 1 << 4;
            case memory_region_kind::free:
            default:
                return 0;
            }
        }

        constexpr NTSTATUS nt_free_virtual_memory_denied_status(const memory_region_kind kind)
        {
            if (is_section_kind(kind))
            {
                return STATUS_UNABLE_TO_DELETE_SECTION;
            }

            if (kind == memory_region_kind::mmio)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
