#pragma once
#include <vector>
#include <functional>

#include "memory_permission.hpp"

using mmio_read_callback = std::function<uint64_t(uint64_t addr, size_t size)>;
using mmio_write_callback = std::function<void(uint64_t addr, size_t size, uint64_t data)>;

class memory_manager;

class memory_interface
{
  public:
    friend memory_manager;

    virtual ~memory_interface() = default;

    virtual void read_memory(uint64_t address, void* data, size_t size) const = 0;
    virtual bool try_read_memory(uint64_t address, void* data, size_t size) const = 0;
    virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;

  private:
    virtual void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) = 0;
    virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
    virtual void unmap_memory(uint64_t address, size_t size) = 0;

    virtual void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) = 0;

  public:
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
};
