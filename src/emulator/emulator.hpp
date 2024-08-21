#pragma once
#include <chrono>
#include <vector>

#include "memory_permission.hpp"

struct memory_region
{
	uint64_t start;
	size_t length;
	memory_permission pemissions;
};

class emulator
{
public:
	emulator() = default;

	emulator(const emulator&) = delete;
	emulator& operator=(const emulator&) = delete;

	emulator(emulator&&) = delete;
	emulator& operator=(emulator&&) = delete;

	virtual ~emulator() = default;

	virtual void start(uint64_t start, uint64_t end = 0, std::chrono::microseconds timeout = {}, size_t count = 0) = 0;
	virtual void stop() = 0;

	virtual void read_raw_register(int reg, void* value, size_t size) = 0;
	virtual void write_raw_register(int reg, const void* value, size_t size) = 0;

	virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
	virtual void unmap_memory(uint64_t address, size_t size) = 0;

	virtual void read_memory(uint64_t address, void* data, size_t size) = 0;
	virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;

	virtual void protect_memory(uint64_t address, size_t size, memory_permission permissions) = 0;

	virtual std::vector<memory_region> get_memory_regions() = 0;
};

template <typename PointerType, typename Register, Register StackPointer>
class typed_emulator : public emulator
{
public:
	using registers = Register;
	using pointer_type = PointerType;

	static constexpr size_t pointer_size = sizeof(pointer_type);
	static constexpr registers stack_pointer = StackPointer;

	void write_register(registers reg, const void* value, const size_t size)
	{
		this->write_raw_register(static_cast<int>(reg), value, size);
	}

	void read_register(registers reg, void* value, const size_t size)
	{
		this->read_raw_register(static_cast<int>(reg), value, size);
	}

	template <typename T = uint64_t>
	T reg(const registers regid) const
	{
		T value{};
		this->read_register(regid, &value, sizeof(value));
		return value;
	}

	template <typename T = uint64_t, typename S>
	void reg(const registers regid, const S& maybe_value) const
	{
		T value = static_cast<T>(maybe_value);
		this->write_register(regid, &value, sizeof(value));
	}

	pointer_type read_stack(const size_t index) const
	{
		uint64_t result{};
		const auto sp = this->reg(stack_pointer);

		this->read_memory(sp + (index * pointer_size), &result, sizeof(result));

		return result;
	}

private:
	void read_raw_register(int reg, void* value, size_t size) override = 0;
	void write_raw_register(int reg, const void* value, size_t size) override = 0;
};
