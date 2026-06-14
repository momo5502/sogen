#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace sogen
{

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

        virtual bool supports_instruction_counting() const = 0;

        // Whether stop() may be safely called from a different thread while the CPU is executing.
        // Hypervisor-backed backends can cancel execution from any thread; the JIT/interpreter
        // backends cannot, so they must be preempted cooperatively from the CPU thread instead.
        virtual bool is_stop_thread_safe() const = 0;
    };

} // namespace sogen
