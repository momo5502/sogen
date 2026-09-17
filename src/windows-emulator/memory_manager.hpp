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

        using memory_interface::read_memory;

        void read_memory(uint64_t address, void* data, size_t size) const final;
        bool try_read_memory(uint64_t address, void* data, size_t size) const final;
        void write_memory(uint64_t address, const void* data, size_t size) final;
        bool try_write_memory(uint64_t address, const void* data, size_t size) final;

        bool protect_memory(uint64_t address, size_t size, nt_memory_permission permissions,
                            nt_memory_permission* old_permissions = nullptr);

        // Pre-reserves the host ranges the backend reports (memory_interface::reserved_host_ranges) so
        // later guest allocations steer clear. Only ever adds ranges, so it is cheap enough to call
        // before every dynamic allocation and can never momentarily drop a reservation.
        void reserve_host_memory_ranges();

        // Windowed form of reserve_host_memory_ranges, for the fixed-address allocate_memory overload,
        // which only needs its own target window checked rather than a full-address-space rescan.
        void reserve_host_memory_ranges_in(uint64_t address, size_t size);

        // Pure probe: true if no foreign host mapping currently intersects [address, size). Unlike
        // reserve_host_memory_ranges_in it records nothing.
        bool host_window_is_free(uint64_t address, size_t size) const;

        // reserve_host_memory_ranges, but releasing every previously-tracked range first. Momentarily
        // un-reserves everything and doubles the syscall cost, so it must stay off hot paths - it is
        // only for when the backend's answer can genuinely change.
        void reset_host_memory_ranges();

        bool allocate_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb);
        bool allocate_host_memory_at(uint64_t address, size_t size, void* host_pointer, nt_memory_permission permissions);
        // Chooses a compatible guest address and aliases it onto caller-owned host memory (e.g. a host Vulkan
        // mapping). The region is treated like MMIO: not serialized, host_pointer not owned. Returns 0 on failure.
        uint64_t allocate_host_memory(size_t size, void* host_pointer, nt_memory_permission permissions);

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

        // find_free_allocation_base, plus a confirmation that the pick is free at the host level and not
        // merely per sogen's own bookkeeping, re-picking past any foreign host mapping that claimed it
        // since the last scan (bounded retry, returns 0 if no pick could be confirmed). Identical to
        // find_free_allocation_base on backends with an independent guest address space.
        uint64_t find_free_host_allocation_base(size_t size, uint64_t start);

        // Same, but capped at highest_address for callers with a hard architectural ceiling (e.g. a
        // below-4GB requirement) where a higher pick would be useless even if free.
        uint64_t find_free_host_allocation_base(size_t size, uint64_t start, uint64_t highest_address);

        region_info get_region_info(uint64_t address);
        std::optional<std::u16string> get_region_mapped_filename(uint64_t address) const;
        void set_region_mapped_filename(uint64_t address, std::u16string filename);

        reserved_region_map::iterator find_reserved_region(uint64_t address);

        // ignore_host_reserved skips memory_region_kind::host_reserved entries - used when mapping an
        // existing host allocation or MMIO, neither of which needs a new host address range.
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
        std::vector<uint64_t> host_reserved_addresses_{};

        void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) final;
        void map_memory(uint64_t address, size_t size, memory_permission permissions) final;
        void map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) final;
        void unmap_memory(uint64_t address, size_t size) final;
        void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) final;

        void update_layout_version();
        bool commit_memory(uint64_t address, size_t size, nt_memory_permission permissions, bool allow_image_section);
        memory_permission get_effective_permissions(nt_memory_permission permissions) const;

        // allocate_memory(address, ...) without the host-range rescan the public overload performs
        // first; reserve_host_memory_ranges calls this to avoid recursing back into itself.
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
