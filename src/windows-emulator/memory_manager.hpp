#pragma once
#include "std_include.hpp"
#include <map>
#include <atomic>
#include <cstdint>

#include "memory_permission_ext.hpp"
#include "memory_region.hpp"
#include "serialization.hpp"

#include <memory_interface.hpp>

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
    section_view,
    section_image,
    mmio,
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

    using committed_region_map = serialized_map<uint64_t, committed_region>;

    struct reserved_region
    {
        size_t length{};
        memory_permission initial_permission{};
        committed_region_map committed_regions{};
        memory_region_kind kind{memory_region_kind::private_allocation};
    };

    using reserved_region_map = serialized_map<uint64_t, reserved_region>;

    void read_memory(uint64_t address, void* data, size_t size) const final;
    bool try_read_memory(uint64_t address, void* data, size_t size) const final;
    void write_memory(uint64_t address, const void* data, size_t size) final;
    bool try_write_memory(uint64_t address, const void* data, size_t size) final;

    bool protect_memory(uint64_t address, size_t size, nt_memory_permission permissions, nt_memory_permission* old_permissions = nullptr);

    bool allocate_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb);
    bool allocate_memory(uint64_t address, size_t size, nt_memory_permission permissions, bool reserve_only = false,
                         memory_region_kind kind = memory_region_kind::private_allocation);

    bool commit_memory(uint64_t address, size_t size, nt_memory_permission permissions);
    bool decommit_memory(uint64_t address, size_t size);

    bool release_memory(uint64_t address, size_t size);

    void unmap_all_memory();

    uint64_t allocate_memory(size_t size, nt_memory_permission permissions, bool reserve_only = false, uint64_t start = 0,
                             memory_region_kind kind = memory_region_kind::private_allocation);

    uint64_t find_free_allocation_base(size_t size, uint64_t start = 0) const;

    region_info get_region_info(uint64_t address);

    reserved_region_map::iterator find_reserved_region(uint64_t address);

    bool overlaps_reserved_region(uint64_t address, size_t size) const;

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

  private:
    memory_interface* memory_{};
    reserved_region_map reserved_regions_{};
    std::atomic<std::uint64_t> layout_version_{0};
    std::uint64_t default_allocation_address_{0x100000000ULL};

    void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) final;
    void map_memory(uint64_t address, size_t size, memory_permission permissions) final;
    void unmap_memory(uint64_t address, size_t size) final;
    void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) final;

    void update_layout_version();
};

namespace memory_region_policy
{
    constexpr bool is_section_kind(const memory_region_kind kind)
    {
        return kind == memory_region_kind::section_view || kind == memory_region_kind::section_image;
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
        case memory_region_kind::section_view:
            return MEM_MAPPED;
        case memory_region_kind::mmio:
            return MEM_MAPPED;
        default:
            return MEM_PRIVATE;
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
