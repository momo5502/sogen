#pragma once

#include "std_include.hpp"

#include <map>
#include <atomic>
#include <cstdint>

#include <memory_permission.hpp>
#include <memory_region.hpp>
#include <memory_interface.hpp>
#include <address_utils.hpp>
#include <serialization.hpp>

// --------------------------------------------------------------------------
// Linux uses a simpler memory model than Windows: no separate reserve/commit,
// no guard pages. This is a simplified memory_manager using plain
// memory_permission rather than nt_memory_permission.
// --------------------------------------------------------------------------

constexpr auto LINUX_PAGE_SIZE = 0x1000ULL;
constexpr auto LINUX_ALLOCATION_GRANULARITY = 0x1000ULL;
constexpr auto LINUX_MIN_MMAP_ADDRESS = 0x0000000000010000ULL;
// Allow higher virtual addresses used by modern Linux loaders/runtime setups
// (e.g. 5-level paging style layouts) instead of limiting to classic 47-bit.
constexpr auto LINUX_MAX_MMAP_ADDRESS = 0x0000ffffffffffffULL;
constexpr auto LINUX_MAX_MMAP_END_EXCL = LINUX_MAX_MMAP_ADDRESS + 1ULL;
constexpr auto LINUX_DEFAULT_MMAP_BASE = 0x7f0000000000ULL;

class linux_memory_manager : public memory_interface
{
  public:
    linux_memory_manager(memory_interface& memory)
        : memory_(&memory)
    {
    }

    struct mapped_region
    {
        size_t length{};
        memory_permission permissions{};
    };

    using mapped_region_map = std::map<uint64_t, mapped_region>;

    // memory_interface
    void read_memory(uint64_t address, void* data, size_t size) const override;
    bool try_read_memory(uint64_t address, void* data, size_t size) const override;
    void write_memory(uint64_t address, const void* data, size_t size) override;
    bool try_write_memory(uint64_t address, const void* data, size_t size) override;

    // Allocate and map a region
    bool allocate_memory(uint64_t address, size_t size, memory_permission permissions);
    uint64_t allocate_memory(size_t size, memory_permission permissions, uint64_t start = 0);

    // Change protection on mapped memory
    bool protect_memory(uint64_t address, size_t size, memory_permission permissions);

    // Unmap a region
    bool release_memory(uint64_t address, size_t size);

    // Unmap everything
    void unmap_all_memory();

    // Find a free region of the given size
    uint64_t find_free_allocation_base(size_t size, uint64_t start = 0) const;

    // Check if a region overlaps any existing mapping
    bool overlaps_mapped_region(uint64_t address, size_t size) const;

    const mapped_region_map& get_mapped_regions() const
    {
        return this->mapped_regions_;
    }

    uint64_t get_mmap_base() const
    {
        return this->mmap_base_;
    }

    void set_mmap_base(uint64_t address)
    {
        this->mmap_base_ = address;
    }

    void serialize_memory_state(utils::buffer_serializer& buffer) const;
    void deserialize_memory_state(utils::buffer_deserializer& buffer);

  private:
    memory_interface* memory_{};
    mapped_region_map mapped_regions_{};
    uint64_t mmap_base_{LINUX_DEFAULT_MMAP_BASE};

    void map_memory(uint64_t address, size_t size, memory_permission permissions) override;
    void unmap_memory(uint64_t address, size_t size) override;
    void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override;
    void map_mmio(uint64_t address, size_t size, std::function<void(uint64_t, void*, size_t)> read_cb,
                  std::function<void(uint64_t, const void*, size_t)> write_cb) override;
};
