#pragma once
#include <map>
#include <atomic>
#include <cstdint>

#include "memory_region.hpp"
#include "address_utils.hpp"
#include "serialization.hpp"

struct region_info : basic_memory_region
{
    uint64_t allocation_base{};
    size_t allocation_length{};
    bool is_reserved{};
    bool is_committed{};
};

using mmio_read_callback = std::function<uint64_t(uint64_t addr, size_t size)>;
using mmio_write_callback = std::function<void(uint64_t addr, size_t size, uint64_t data)>;

class memory_manager
{
  public:
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

    virtual ~memory_manager() = default;

    template <typename T>
    T read_memory(const uint64_t address) const
    {
        T value{};
        this->read_memory(address, &value, sizeof(value));
        return value;
    }

    template <typename T>
    T read_memory(const void* address) const
    {
        return this->read_memory<T>(reinterpret_cast<uint64_t>(address));
    }

    std::vector<std::byte> read_memory(const uint64_t address, const size_t size) const
    {
        std::vector<std::byte> data{};
        data.resize(size);

        this->read_memory(address, data.data(), data.size());

        return data;
    }

    std::vector<std::byte> read_memory(const void* address, const size_t size) const
    {
        return this->read_memory(reinterpret_cast<uint64_t>(address), size);
    }

    template <typename T>
    void write_memory(const uint64_t address, const T& value)
    {
        this->write_memory(address, &value, sizeof(value));
    }

    template <typename T>
    void write_memory(void* address, const T& value)
    {
        this->write_memory(reinterpret_cast<uint64_t>(address), &value, sizeof(value));
    }

    void write_memory(void* address, const void* data, const size_t size)
    {
        this->write_memory(reinterpret_cast<uint64_t>(address), data, size);
    }

    virtual void read_memory(uint64_t address, void* data, size_t size) const = 0;
    virtual bool try_read_memory(uint64_t address, void* data, size_t size) const = 0;
    virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;

    bool protect_memory(uint64_t address, size_t size, memory_permission permissions,
                        memory_permission* old_permissions = nullptr);

    bool allocate_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb);
    bool allocate_memory(uint64_t address, size_t size, memory_permission permissions, bool reserve_only = false);

    bool commit_memory(uint64_t address, size_t size, memory_permission permissions);
    bool decommit_memory(uint64_t address, size_t size);

    bool release_memory(uint64_t address, size_t size);

    uint64_t allocate_memory(const size_t size, const memory_permission permissions, const bool reserve_only = false)
    {
        const auto allocation_base = this->find_free_allocation_base(size);
        if (!allocate_memory(allocation_base, size, permissions, reserve_only))
        {
            return 0;
        }

        return allocation_base;
    }

    uint64_t find_free_allocation_base(size_t size, uint64_t start = 0) const;

    region_info get_region_info(uint64_t address);

    reserved_region_map::iterator find_reserved_region(uint64_t address);

    bool overlaps_reserved_region(uint64_t address, size_t size) const;

    const reserved_region_map& get_reserved_regions() const
    {
        return reserved_regions_;
    }

    std::uint64_t get_memory_layout_state_ver() const
    {
        return memory_layout_state_version_.load(std::memory_order_relaxed);
    }

  private:
    reserved_region_map reserved_regions_{};

    virtual void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) = 0;
    virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
    virtual void unmap_memory(uint64_t address, size_t size) = 0;

    virtual void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) = 0;

  protected:
    std::atomic<std::uint64_t> memory_layout_state_version_{0};

    void invalidate_memory_layout_state_version()
    {
#if MOMO_REFLECTION_LEVEL > 0
        memory_layout_state_version_.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    void serialize_memory_state(utils::buffer_serializer& buffer, bool is_snapshot) const;
    void deserialize_memory_state(utils::buffer_deserializer& buffer, bool is_snapshot);
};
