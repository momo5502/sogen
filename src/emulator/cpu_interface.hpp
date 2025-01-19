#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>

struct cpu_interface
{
    virtual ~cpu_interface() = default;

    virtual void start(uint64_t start, uint64_t end = 0, std::chrono::nanoseconds timeout = {}, size_t count = 0) = 0;
    virtual void stop() = 0;

    virtual size_t read_raw_register(int reg, void* value, size_t size) = 0;
    virtual size_t write_raw_register(int reg, const void* value, size_t size) = 0;

    virtual std::vector<std::byte> save_registers() = 0;
    virtual void restore_registers(const std::vector<std::byte>& register_data) = 0;

    virtual bool has_violation() const = 0;
};
