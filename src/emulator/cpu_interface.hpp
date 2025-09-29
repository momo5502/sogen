#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>

struct cpu_interface
{
    virtual ~cpu_interface() = default;

    struct descriptor_table_register
    {
        uint64_t base{};
        uint32_t limit{};
    };

    virtual bool read_descriptor_table(int reg, descriptor_table_register& table) = 0;

    virtual void start(size_t count = 0) = 0;
    virtual void stop() = 0;

    virtual size_t read_raw_register(int reg, void* value, size_t size) = 0;
    virtual size_t write_raw_register(int reg, const void* value, size_t size) = 0;

    virtual std::vector<std::byte> save_registers() const = 0;
    virtual void restore_registers(const std::vector<std::byte>& register_data) = 0;

    // TODO: Remove this
    virtual bool has_violation() const = 0;
};
