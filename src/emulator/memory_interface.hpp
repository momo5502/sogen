#pragma once
#include <vector>
#include <functional>

#include "memory_permission.hpp"

using mmio_read_callback = std::function<void(uint64_t addr, void* data, size_t size)>;
using mmio_write_callback = std::function<void(uint64_t addr, const void* data, size_t size)>;

class memory_manager;

class memory_interface
{
  public:
    friend memory_manager;

    virtual ~memory_interface() = default;

    virtual void read_memory(uint64_t address, void* data, size_t size) const = 0;
    virtual bool try_read_memory(uint64_t address, void* data, size_t size) const = 0;
    virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;
    virtual bool try_write_memory(uint64_t address, const void* data, size_t size) = 0;

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

    void move_memory(uint64_t dst, uint64_t src, size_t size)
    {
        if (dst == src || !size)
        {
            return;
        }

        const auto copy_from_end = src < dst;
        const auto increment = copy_from_end ? -1 : 1;

        auto p_src = copy_from_end ? src + size - 1 : src;
        auto p_dst = copy_from_end ? dst + size - 1 : dst;

        while (size--)
        {
            const auto elem = this->read_memory<std::byte>(p_src);
            this->write_memory(p_dst, elem);
            p_src += increment;
            p_dst += increment;
        }
    }

    void move_memory(void* dst, const void* src, size_t size)
    {
        this->move_memory(reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), size);
    }
};
