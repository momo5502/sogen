#pragma once
#include <chrono>
#include <functional>
#include <cassert>

#include "memory_manager.hpp"

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

using edge_generation_hook_callback = std::function<void(const basic_block& current_block,
                                                         const basic_block& previous_block)>;
using basic_block_hook_callback = std::function<void(const basic_block& block)>;

using instruction_hook_callback = std::function<instruction_hook_continuation()>;

using interrupt_hook_callback = std::function<void(int interrupt)>;
using simple_memory_hook_callback = std::function<void(uint64_t address, size_t size, uint64_t value)>;
using complex_memory_hook_callback = std::function<void(uint64_t address, size_t size, uint64_t value,
                                                        memory_operation operation)>;
using memory_violation_hook_callback = std::function<memory_violation_continuation(
	uint64_t address, size_t size, memory_operation operation,
	memory_violation_type type)>;

class emulator : public memory_manager
{
public:
	emulator() = default;

	emulator(const emulator&) = delete;
	emulator& operator=(const emulator&) = delete;

	emulator(emulator&&) = delete;
	emulator& operator=(emulator&&) = delete;

	virtual void start(uint64_t start, uint64_t end = 0, std::chrono::nanoseconds timeout = {}, size_t count = 0) = 0;
	virtual void stop() = 0;

	virtual void read_raw_register(int reg, void* value, size_t size) = 0;
	virtual void write_raw_register(int reg, const void* value, size_t size) = 0;

	virtual std::vector<std::byte> save_registers() = 0;
	virtual void restore_registers(const std::vector<std::byte>& register_data) = 0;

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

	void serialize(utils::buffer_serializer& buffer) const
	{
		this->perform_serialization(buffer, false);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		this->perform_deserialization(buffer, false);
	}

	void save_snapshot()
	{
		utils::buffer_serializer serializer{};
		this->perform_serialization(serializer, true);
		this->last_snapshot_data_ = serializer.move_buffer();
	}

	void restore_snapshot()
	{
		if (this->last_snapshot_data_.empty())
		{
			return;
		}

		utils::buffer_deserializer deserializer{this->last_snapshot_data_};
		this->perform_deserialization(deserializer, true);
	}

	virtual bool has_violation() const = 0;

private:
	std::vector<std::byte> last_snapshot_data_{};

	emulator_hook* hook_simple_memory_access(const uint64_t address, const size_t size,
	                                         simple_memory_hook_callback callback, const memory_operation operation)
	{
		assert((static_cast<uint8_t>(operation) & (static_cast<uint8_t>(operation) - 1)) == 0);
		return this->hook_memory_access(address, size, operation,
		                                [c = std::move(callback)](const uint64_t a, const size_t s,
		                                                          const uint64_t value,
		                                                          memory_operation)
		                                {
			                                c(a, s, value);
		                                });
	}

	void perform_serialization(utils::buffer_serializer& buffer, const bool is_snapshot) const
	{
		this->serialize_state(buffer, is_snapshot);
		this->serialize_memory_state(buffer, is_snapshot);
	}

	void perform_deserialization(utils::buffer_deserializer& buffer, const bool is_snapshot)
	{
		this->deserialize_state(buffer, is_snapshot);
		this->deserialize_memory_state(buffer, is_snapshot);
	}

	virtual void serialize_state(utils::buffer_serializer& buffer, bool is_snapshot) const = 0;
	virtual void deserialize_state(utils::buffer_deserializer& buffer, bool is_snapshot) = 0;
};
