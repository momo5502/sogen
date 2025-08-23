#pragma once

#include "emulator.hpp"

template <typename Traits>
class typed_emulator : public emulator
{
  public:
    using registers = typename Traits::register_type;
    using pointer_type = typename Traits::pointer_type;
    using hookable_instructions = typename Traits::hookable_instructions;

    static constexpr size_t pointer_size = sizeof(pointer_type);
    static constexpr registers stack_pointer = Traits::stack_pointer;
    static constexpr registers instruction_pointer = Traits::instruction_pointer;

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

    emulator_hook* hook_instruction(hookable_instructions instruction_type, simple_instruction_hook_callback callback)
    {
        return this->hook_instruction(instruction_type, [c = std::move(callback)](const uint64_t) {
            return c(); //
        });
    }

  private:
    emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override = 0;

    size_t read_raw_register(int reg, void* value, size_t size) override = 0;
    size_t write_raw_register(int reg, const void* value, size_t size) override = 0;
};
