#pragma once

#include "emulator.hpp"

template <typename PointerType, typename Register, Register InstructionPointer, Register StackPointer,
          typename HookableInstructions>
class typed_emulator : public emulator
{
  public:
    using registers = Register;
    using pointer_type = PointerType;
    using hookable_instructions = HookableInstructions;

    static constexpr size_t pointer_size = sizeof(pointer_type);
    static constexpr registers stack_pointer = StackPointer;
    static constexpr registers instruction_pointer = InstructionPointer;

    void start_from_ip(const std::chrono::nanoseconds timeout = {}, const size_t count = 0)
    {
        this->start(this->read_instruction_pointer(), 0, timeout, count);
    }

    size_t write_register(registers reg, const void* value, const size_t size)
    {
        return this->write_raw_register(static_cast<int>(reg), value, size);
    }

    size_t read_register(registers reg, void* value, const size_t size)
    {
        return this->read_raw_register(static_cast<int>(reg), value, size);
    }

    template <typename T = pointer_type>
    T reg(const registers regid)
    {
        T value{};
        this->read_register(regid, &value, sizeof(value));
        return value;
    }

    template <typename T = pointer_type, typename S>
    void reg(const registers regid, const S& maybe_value)
    {
        T value = static_cast<T>(maybe_value);
        this->write_register(regid, &value, sizeof(value));
    }

    pointer_type read_instruction_pointer()
    {
        return this->reg(instruction_pointer);
    }

    pointer_type read_stack_pointer()
    {
        return this->reg(stack_pointer);
    }

    pointer_type read_stack(const size_t index)
    {
        pointer_type result{};
        const auto sp = this->read_stack_pointer();

        this->read_memory(sp + (index * pointer_size), &result, sizeof(result));

        return result;
    }

    void push_stack(const pointer_type& value)
    {
        const auto sp = this->read_stack_pointer() - pointer_size;
        this->reg(stack_pointer, sp);
        this->write_memory(sp, &value, sizeof(value));
    }

    pointer_type pop_stack()
    {
        pointer_type result{};
        const auto sp = this->read_stack_pointer();
        this->read_memory(sp, &result, sizeof(result));
        this->reg(stack_pointer, sp + pointer_size);

        return result;
    }

    emulator_hook* hook_instruction(hookable_instructions instruction_type, instruction_hook_callback callback)
    {
        return this->hook_instruction(static_cast<int>(instruction_type), std::move(callback));
    }

  private:
    emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override = 0;

    size_t read_raw_register(int reg, void* value, size_t size) override = 0;
    size_t write_raw_register(int reg, const void* value, size_t size) override = 0;
};
