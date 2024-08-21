#pragma once

#include "emulator.hpp"

using simple_instruction_hook_callback = std::function<void()>;

template <typename PointerType, typename Register, Register InstructionPointer, Register
          StackPointer, typename HookableInstructions>
class typed_emulator : public emulator
{
public:
	using registers = Register;
	using pointer_type = PointerType;
	using hookable_instructions = HookableInstructions;

	static constexpr size_t pointer_size = sizeof(pointer_type);
	static constexpr registers stack_pointer = StackPointer;
	static constexpr registers instruction_pointer = InstructionPointer;

	void write_register(registers reg, const void* value, const size_t size)
	{
		this->write_raw_register(static_cast<int>(reg), value, size);
	}

	void read_register(registers reg, void* value, const size_t size)
	{
		this->read_raw_register(static_cast<int>(reg), value, size);
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

	pointer_type read_stack(const size_t index)
	{
		pointer_type result{};
		const auto sp = this->reg(stack_pointer);

		this->read_memory(sp + (index * pointer_size), &result, sizeof(result));

		return result;
	}

	emulator_hook* hook_instruction(hookable_instructions instruction_type, instruction_hook_callback callback)
	{
		return this->hook_instruction(instruction_type, [this, c = std::move(callback)]
		{
			const auto ip = static_cast<uint64_t>(this->reg(instruction_pointer));
			c(ip);
		});
	}

	virtual emulator_hook* hook_instruction(hookable_instructions instruction_type,
	                                        simple_instruction_hook_callback callback) = 0;

private:
	emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
	{
		return this->hook_instruction(static_cast<hookable_instructions>(instruction_type), std::move(callback));
	}

	void read_raw_register(int reg, void* value, size_t size) override = 0;
	void write_raw_register(int reg, const void* value, size_t size) override = 0;
};
