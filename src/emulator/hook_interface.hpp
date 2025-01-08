#pragma once

#include "memory_permission.hpp"

#include <cstddef>
#include <cassert>
#include <functional>

struct emulator_hook;

using memory_operation = memory_permission;

enum class instruction_hook_continuation : bool
{
    run_instruction = false,
    skip_instruction = true,
};

enum class memory_violation_continuation : bool
{
    stop = false,
    resume = true,
};

enum class memory_violation_type : uint8_t
{
    unmapped,
    protection,
};

struct basic_block
{
    uint64_t address;
    size_t instruction_count;
    size_t size;
};

using edge_generation_hook_callback =
    std::function<void(const basic_block& current_block, const basic_block& previous_block)>;
using basic_block_hook_callback = std::function<void(const basic_block& block)>;

using instruction_hook_callback = std::function<instruction_hook_continuation()>;

using interrupt_hook_callback = std::function<void(int interrupt)>;
using simple_memory_hook_callback = std::function<void(uint64_t address, size_t size, uint64_t value)>;
using complex_memory_hook_callback =
    std::function<void(uint64_t address, size_t size, uint64_t value, memory_operation operation)>;
using memory_violation_hook_callback = std::function<memory_violation_continuation(
    uint64_t address, size_t size, memory_operation operation, memory_violation_type type)>;

class hook_interface
{
  public:
    virtual ~hook_interface() = default;

    virtual emulator_hook* hook_memory_violation(uint64_t address, size_t size,
                                                 memory_violation_hook_callback callback) = 0;

    virtual emulator_hook* hook_memory_access(uint64_t address, size_t size, memory_operation filter,
                                              complex_memory_hook_callback callback) = 0;
    virtual emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) = 0;

    virtual emulator_hook* hook_interrupt(interrupt_hook_callback callback) = 0;

    virtual emulator_hook* hook_edge_generation(edge_generation_hook_callback callback) = 0;
    virtual emulator_hook* hook_basic_block(basic_block_hook_callback callback) = 0;

    virtual void delete_hook(emulator_hook* hook) = 0;

    emulator_hook* hook_memory_violation(memory_violation_hook_callback callback)
    {
        return this->hook_memory_violation(0, std::numeric_limits<size_t>::max(), std::move(callback));
    }

    emulator_hook* hook_memory_read(const uint64_t address, const size_t size, simple_memory_hook_callback callback)
    {
        return this->hook_simple_memory_access(address, size, std::move(callback), memory_operation::read);
    }

    emulator_hook* hook_memory_write(const uint64_t address, const size_t size, simple_memory_hook_callback callback)
    {
        return this->hook_simple_memory_access(address, size, std::move(callback), memory_operation::write);
    }

    emulator_hook* hook_memory_execution(const uint64_t address, const size_t size,
                                         simple_memory_hook_callback callback)
    {
        return this->hook_simple_memory_access(address, size, std::move(callback), memory_operation::exec);
    }

  private:
    emulator_hook* hook_simple_memory_access(const uint64_t address, const size_t size,
                                             simple_memory_hook_callback callback, const memory_operation operation)
    {
        assert((static_cast<uint8_t>(operation) & (static_cast<uint8_t>(operation) - 1)) == 0);
        return this->hook_memory_access(address, size, operation,
                                        [c = std::move(callback)](const uint64_t a, const size_t s,
                                                                  const uint64_t value,
                                                                  memory_operation) { c(a, s, value); });
    }
};
