#pragma once
#include <chrono>
#include <vector>
#include <functional>
#include <cassert>

#include "memory_region.hpp"

struct emulator_hook;

using memory_operation = memory_permission;

using instruction_hook_callback = std::function<void(uint64_t address)>;

using simple_memory_hook_callback = std::function<void(uint64_t address, size_t size)>;
using complex_memory_hook_callback = std::function<void(uint64_t address, size_t size, memory_operation operation)>;

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

	virtual bool try_map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
	virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
	virtual void unmap_memory(uint64_t address, size_t size) = 0;

	virtual void read_memory(uint64_t address, void* data, size_t size) = 0;
	virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;

	virtual void protect_memory(uint64_t address, size_t size, memory_permission permissions) = 0;

	virtual std::vector<memory_region> get_memory_regions() = 0;

	virtual emulator_hook* hook_memory_access(uint64_t address, size_t size, memory_operation filter,
	                                          complex_memory_hook_callback callback) = 0;
	virtual emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) = 0;

	virtual void delete_hook(emulator_hook* hook) = 0;

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
		                                                          memory_operation)
		                                {
			                                c(a, s);
		                                });
	}
};
