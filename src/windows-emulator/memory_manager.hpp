#pragma once
#include <map>
#include <atomic>
#include <cstdint>

#include "memory_region.hpp"
#include "serialization.hpp"

#include <memory_interface.hpp>

constexpr auto ALLOCATION_GRANULARITY = 0x0000000000010000ULL;
constexpr auto MIN_ALLOCATION_ADDRESS = 0x0000000000010000ULL;
constexpr auto MAX_ALLOCATION_ADDRESS = 0x00007ffffffeffffULL;

struct region_info : basic_memory_region
{
    uint64_t allocation_base{};
    size_t allocation_length{};
    bool is_reserved{};
    bool is_committed{};
};

using mmio_read_callback = std::function<uint64_t(uint64_t addr, size_t size)>;
using mmio_write_callback = std::function<void(uint64_t addr, size_t size, uint64_t data)>;

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
        memory_permission permissions{};
    };

    using committed_region_map = std::map<uint64_t, committed_region>;

    struct reserved_region
    {
        size_t length{};
        committed_region_map committed_regions{};
        bool is_mmio{false};
    };

    using reserved_region_map = std::map<uint64_t, reserved_region>;

    void read_memory(uint64_t address, void* data, size_t size) const final;
    bool try_read_memory(uint64_t address, void* data, size_t size) const final;
    void write_memory(uint64_t address, const void* data, size_t size) final;

    bool protect_memory(uint64_t address, size_t size, memory_permission permissions,
                        memory_permission* old_permissions = nullptr);

    bool allocate_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb);
    bool allocate_memory(uint64_t address, size_t size, memory_permission permissions, bool reserve_only = false);

    bool commit_memory(uint64_t address, size_t size, memory_permission permissions);
    bool decommit_memory(uint64_t address, size_t size);

    bool release_memory(uint64_t address, size_t size);

    void unmap_all_memory();

    uint64_t allocate_memory(size_t size, memory_permission permissions, bool reserve_only = false);

    uint64_t find_free_allocation_base(size_t size, uint64_t start = 0) const;

    region_info get_region_info(uint64_t address);

    reserved_region_map::iterator find_reserved_region(uint64_t address);

    bool overlaps_reserved_region(uint64_t address, size_t size) const;

    const reserved_region_map& get_reserved_regions() const
    {
        return this->reserved_regions_;
    }

    std::uint64_t get_layout_version() const
    {
        return this->layout_version_.load(std::memory_order_relaxed);
    }

    void serialize_memory_state(utils::buffer_serializer& buffer, bool is_snapshot) const;
    void deserialize_memory_state(utils::buffer_deserializer& buffer, bool is_snapshot);

  private:
    memory_interface* memory_{};
    reserved_region_map reserved_regions_{};
    std::atomic<std::uint64_t> layout_version_{0};

    void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) final;
    void map_memory(uint64_t address, size_t size, memory_permission permissions) final;
    void unmap_memory(uint64_t address, size_t size) final;
    void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) final;

    void update_layout_version();
};
