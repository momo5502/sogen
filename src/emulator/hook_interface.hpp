#pragma once

#include "memory_permission.hpp"

#include <cstddef>
#include <cassert>
#include <functional>
#include <stdexcept>

namespace sogen
{

struct emulator_hook;

using memory_operation = memory_permission;

enum class instruction_hook_continuation : bool
{
    run_instruction = false,
    skip_instruction = true,
};

enum class memory_violation_continuation : uint8_t
{
    stop,
    resume,
    restart,
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

using edge_generation_hook_callback = std::function<void(const basic_block& current_block, const basic_block& previous_block)>;
using basic_block_hook_callback = std::function<void(const basic_block& block)>;

using simple_instruction_hook_callback = std::function<instruction_hook_continuation()>;
using instruction_hook_callback = std::function<instruction_hook_continuation(uint64_t data)>;
using interrupt_hook_callback = std::function<void(int interrupt)>;

using memory_access_hook_callback = std::function<void(uint64_t address, const void* data, size_t size)>;
using memory_execution_hook_callback = std::function<void(uint64_t address)>;

using memory_violation_hook_callback =
    std::function<memory_violation_continuation(uint64_t address, size_t size, memory_operation operation, memory_violation_type type)>;

class hook_interface
{
  public:
    virtual ~hook_interface() = default;

    enum class memory_execution_hook_mode
    {
        automatic,
        int3,
    };

    virtual void set_memory_execution_hook_mode(const memory_execution_hook_mode mode)
    {
        if (mode == memory_execution_hook_mode::int3)
        {
            throw std::runtime_error("The selected emulator backend does not support int3 memory execution hooks");
        }
    }

    virtual emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) = 0;
    virtual emulator_hook* hook_memory_execution(uint64_t address, memory_execution_hook_callback callback) = 0;
    virtual emulator_hook* hook_memory_read(uint64_t address, uint64_t size, memory_access_hook_callback callback) = 0;
    virtual emulator_hook* hook_memory_write(uint64_t address, uint64_t size, memory_access_hook_callback callback) = 0;

    virtual emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) = 0;

    virtual emulator_hook* hook_interrupt(interrupt_hook_callback callback) = 0;
    virtual emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) = 0;

    virtual emulator_hook* hook_basic_block(basic_block_hook_callback callback) = 0;

    virtual void delete_hook(emulator_hook* hook) = 0;
};

} // namespace sogen

