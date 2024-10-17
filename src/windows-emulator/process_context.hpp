#pragma once

#include "emulator_utils.hpp"
#include "handles.hpp"

#include "module/module_manager.hpp"
#include <utils/nt_handle.hpp>

#include <x64_emulator.hpp>

#define PEB_SEGMENT_SIZE (1 << 20) // 1 MB
#define GS_SEGMENT_SIZE (1 << 20) // 1 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define KUSD_ADDRESS 0x7ffe0000

#define STACK_SIZE 0x40000ULL

#define GDT_ADDR 0x30000
#define GDT_LIMIT 0x1000
#define GDT_ENTRY_SIZE 0x8

struct event
{
	bool signaled{};
	EVENT_TYPE type{};
	std::wstring name{};
	uint32_t ref_count{0};

	bool is_signaled()
	{
		const auto res = this->signaled;

		if (this->type == SynchronizationEvent)
		{
			this->signaled = false;
		}

		return res;
	}

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->signaled);
		buffer.write(this->type);
		buffer.write(this->name);
		buffer.write(this->ref_count);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->signaled);
		buffer.read(this->type);
		buffer.read(this->name);
		buffer.read(this->ref_count);
	}

	static bool deleter(event& e)
	{
		return --e.ref_count == 0;
	}
};

struct file
{
	utils::nt::handle<utils::nt::invalid_handle> handle{};
	std::wstring name{};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->name);
		// TODO: Serialize handle
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->name);
		this->handle = INVALID_HANDLE_VALUE;
	}
};

struct semaphore
{
	std::wstring name{};
	volatile uint32_t current_count{};
	uint32_t max_count{};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->name);
		buffer.write(this->current_count);
		buffer.write(this->max_count);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->name);
		buffer.read(this->current_count);
		buffer.read(this->max_count);
	}
};

struct port
{
	std::wstring name{};
	uint64_t view_base{};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->name);
		buffer.write(this->view_base);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->name);
		buffer.read(this->view_base);
	}
};

struct process_context;

class moved_marker
{
public:
	moved_marker() = default;

	moved_marker(const moved_marker& copy) = default;
	moved_marker& operator=(const moved_marker&) = default;

	moved_marker(moved_marker&& obj) noexcept
		: moved_marker()
	{
		this->operator=(std::move(obj));
	}

	moved_marker& operator=(moved_marker&& obj) noexcept
	{
		if (this != &obj)
		{
			this->was_moved_ = obj.was_moved_;
			obj.was_moved_ = true;
		}

		return *this;
	}

	~moved_marker() = default;

	bool was_moved() const
	{
		return this->was_moved_;
	}

private:
	bool was_moved_{false};
};

class emulator_thread
{
public:
	emulator_thread() = default;

	emulator_thread(x64_emulator& emu, const process_context& context, uint64_t start_address, uint64_t argument,
	                uint64_t stack_size, uint32_t id);

	emulator_thread(const emulator_thread&) = delete;
	emulator_thread& operator=(const emulator_thread&) = delete;

	emulator_thread(emulator_thread&& obj) noexcept = default;
	emulator_thread& operator=(emulator_thread&& obj) noexcept = default;

	~emulator_thread()
	{
		if (marker.was_moved())
		{
			return;
		}

		if (this->stack_base)
		{
			this->emu_ptr->release_memory(this->stack_base, this->stack_size);
		}

		if (this->gs_segment)
		{
			this->gs_segment->release();
		}
	}

	moved_marker marker{};

	x64_emulator* emu_ptr{};

	uint64_t stack_base{};
	uint64_t stack_size{};
	uint64_t start_address{};
	uint64_t argument{};
	uint64_t executed_instructions{0};

	uint32_t id{};

	std::optional<handle> await_object{};

	std::optional<emulator_allocator> gs_segment;
	std::optional<emulator_object<TEB>> teb;

	std::vector<std::byte> last_registers{};

	void save(x64_emulator& emu)
	{
		this->last_registers = emu.save_registers();
	}

	void restore(x64_emulator& emu) const
	{
		emu.restore_registers(this->last_registers);
	}

	void setup_if_necessary(x64_emulator& emu, const process_context& context) const
	{
		if (!this->executed_instructions)
		{
			this->setup_registers(emu, context);
		}
	}

	void serialize(utils::buffer_serializer&) const
	{
		// TODO
	}

	void deserialize(utils::buffer_deserializer&)
	{
		// TODO
	}

private:
	void setup_registers(x64_emulator& emu, const process_context& context) const;
};

struct process_context
{
	process_context(x64_emulator& emu)
		: base_allocator(emu)
		  , peb(emu)
		  , process_params(emu)
		  , kusd(emu)
		  , module_manager(emu)
	{
	}

	uint64_t executed_instructions{0};
	uint64_t current_ip{0};
	uint64_t previous_ip{0};

	std::optional<uint64_t> exception_rip{};

	emulator_allocator base_allocator;

	emulator_object<PEB> peb;
	emulator_object<RTL_USER_PROCESS_PARAMETERS> process_params;
	emulator_object<KUSER_SHARED_DATA> kusd;

	module_manager module_manager;

	mapped_module* executable{};
	mapped_module* ntdll{};
	mapped_module* win32u{};

	uint64_t ldr_initialize_thunk{};
	uint64_t rtl_user_thread_start{};
	uint64_t ki_user_exception_dispatcher{};

	uint64_t shared_section_size{};

	handle_store<handle_types::event, event> events{};
	handle_store<handle_types::file, file> files{};
	handle_store<handle_types::semaphore, semaphore> semaphores{};
	handle_store<handle_types::port, port> ports{};
	std::map<uint16_t, std::wstring> atoms{};

	std::vector<std::byte> default_register_set{};

	uint32_t current_thread_id{0};
	handle_store<handle_types::thread, emulator_thread> threads{};
	emulator_thread* active_thread{nullptr};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->executed_instructions);
		buffer.write(this->current_ip);
		buffer.write(this->previous_ip);
		buffer.write_optional(this->exception_rip);
		buffer.write(this->peb);
		buffer.write(this->process_params);
		buffer.write(this->kusd);
		buffer.write(this->module_manager);

		buffer.write(this->executable->image_base);
		buffer.write(this->ntdll->image_base);
		buffer.write(this->win32u->image_base);

		buffer.write(this->ki_user_exception_dispatcher);

		buffer.write(this->shared_section_size);
		buffer.write(this->events);
		buffer.write(this->files);
		buffer.write(this->semaphores);
		buffer.write(this->ports);
		buffer.write_map(this->atoms);

		// TODO: Serialize/deserialize threads
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->executed_instructions);
		buffer.read(this->current_ip);
		buffer.read(this->previous_ip);
		buffer.read_optional(this->exception_rip);
		buffer.read(this->peb);
		buffer.read(this->process_params);
		buffer.read(this->kusd);
		buffer.read(this->module_manager);

		const auto executable_base = buffer.read<uint64_t>();
		const auto ntdll_base = buffer.read<uint64_t>();
		const auto win32u_base = buffer.read<uint64_t>();

		this->executable = this->module_manager.find_by_address(executable_base);
		this->ntdll = this->module_manager.find_by_address(ntdll_base);
		this->win32u = this->module_manager.find_by_address(win32u_base);

		buffer.read(this->ki_user_exception_dispatcher);

		buffer.read(this->shared_section_size);
		buffer.read(this->events);
		buffer.read(this->files);
		buffer.read(this->semaphores);
		buffer.read(this->ports);
		buffer.read_map(this->atoms);
	}

	handle create_thread(x64_emulator& emu, const uint64_t start_address, const uint64_t argument,
	                     const uint64_t stack_size)
	{
		emulator_thread t{emu, *this, start_address, argument, stack_size, ++this->current_thread_id};
		return this->threads.store(std::move(t));
	}
};
