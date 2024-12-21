#pragma once

#include "emulator_utils.hpp"
#include "handles.hpp"
#include "registry/registry_manager.hpp"

#include "module/module_manager.hpp"
#include <utils/nt_handle.hpp>
#include <utils/file_handle.hpp>

#include <x64_emulator.hpp>
#include <serialization_helper.hpp>

#include "io_device.hpp"
#include "kusd_mmio.hpp"

#define PEB_SEGMENT_SIZE (20 << 20) // 20 MB
#define GS_SEGMENT_SIZE (1 << 20) // 1 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_SIZE 0x40000ULL

#define GDT_ADDR 0x30000
#define GDT_LIMIT 0x1000
#define GDT_ENTRY_SIZE 0x8

class windows_emulator;

struct ref_counted_object
{
	uint32_t ref_count{1};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->ref_count);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->ref_count);
	}

	static bool deleter(ref_counted_object& e)
	{
		return --e.ref_count == 0;
	}
};

struct event : ref_counted_object
{
	bool signaled{};
	EVENT_TYPE type{};
	std::wstring name{};

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

		ref_counted_object::serialize(buffer);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->signaled);
		buffer.read(this->type);
		buffer.read(this->name);

		ref_counted_object::deserialize(buffer);
	}
};

struct file
{
	utils::file_handle handle{};
	std::wstring name{};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->name);
		// TODO: Serialize handle
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->name);
		this->handle = {};
	}
};

struct section
{
	std::wstring name{};
	std::wstring file_name{};
	uint64_t maximum_size{};
	uint32_t section_page_protection{};
	uint32_t allocation_attributes{};

	bool is_image() const
	{
		return this->allocation_attributes & SEC_IMAGE;
	}

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->name);
		buffer.write(this->file_name);
		buffer.write(this->maximum_size);
		buffer.write(this->section_page_protection);
		buffer.write(this->allocation_attributes);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->name);
		buffer.read(this->file_name);
		buffer.read(this->maximum_size);
		buffer.read(this->section_page_protection);
		buffer.read(this->allocation_attributes);
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

class emulator_thread : ref_counted_object
{
public:
	emulator_thread(x64_emulator& emu)
		: emu_ptr(&emu)
	{
	}

	emulator_thread(utils::buffer_deserializer& buffer)
		: emulator_thread(buffer.read<x64_emulator_wrapper>().get())
	{
	}

	emulator_thread(x64_emulator& emu, const process_context& context, uint64_t start_address, uint64_t argument,
	                uint64_t stack_size, uint32_t id);

	emulator_thread(const emulator_thread&) = delete;
	emulator_thread& operator=(const emulator_thread&) = delete;

	emulator_thread(emulator_thread&& obj) noexcept = default;
	emulator_thread& operator=(emulator_thread&& obj) noexcept = default;

	~emulator_thread()
	{
		this->release();
	}

	moved_marker marker{};

	x64_emulator* emu_ptr{};

	uint64_t stack_base{};
	uint64_t stack_size{};
	uint64_t start_address{};
	uint64_t argument{};
	uint64_t executed_instructions{0};

	uint32_t id{};

	std::wstring name{};

	std::optional<NTSTATUS> exit_status{};
	std::optional<handle> await_object{};
	bool waiting_for_alert{false};
	bool alerted{false};
	std::optional<std::chrono::steady_clock::time_point> await_time{};

	std::optional<NTSTATUS> pending_status{};

	std::optional<emulator_allocator> gs_segment;
	std::optional<emulator_object<TEB>> teb;

	std::vector<std::byte> last_registers{};

	void mark_as_ready(NTSTATUS status);

	bool is_await_time_over() const
	{
		return this->await_time.has_value() && this->await_time.value() < std::chrono::steady_clock::now();
	}

	bool is_thread_ready(windows_emulator& win_emu);

	void save(x64_emulator& emu)
	{
		this->last_registers = emu.save_registers();
	}

	void restore(x64_emulator& emu) const
	{
		emu.restore_registers(this->last_registers);
	}

	void setup_if_necessary(x64_emulator& emu, const process_context& context)
	{
		if (!this->executed_instructions)
		{
			this->setup_registers(emu, context);
		}

		if (this->pending_status.has_value())
		{
			const auto status = *this->pending_status;
			this->pending_status = {};

			emu.reg<uint64_t>(x64_register::rax, static_cast<uint64_t>(status));
		}
	}

	void serialize(utils::buffer_serializer& buffer) const
	{
		if (this->marker.was_moved())
		{
			throw std::runtime_error("Object was moved!");
		}

		buffer.write(this->stack_base);
		buffer.write(this->stack_size);
		buffer.write(this->start_address);
		buffer.write(this->argument);
		buffer.write(this->executed_instructions);
		buffer.write(this->id);

		buffer.write_string(this->name);

		buffer.write_optional(this->exit_status);
		buffer.write_optional(this->await_object);

		buffer.write(this->waiting_for_alert);
		buffer.write(this->alerted);

		buffer.write_optional(this->await_time);
		buffer.write_optional(this->pending_status);
		buffer.write_optional(this->gs_segment);
		buffer.write_optional(this->teb);

		buffer.write_vector(this->last_registers);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		if (this->marker.was_moved())
		{
			throw std::runtime_error("Object was moved!");
		}

		this->release();

		buffer.read(this->stack_base);
		buffer.read(this->stack_size);
		buffer.read(this->start_address);
		buffer.read(this->argument);
		buffer.read(this->executed_instructions);
		buffer.read(this->id);

		buffer.read_string(this->name);

		buffer.read_optional(this->exit_status);
		buffer.read_optional(this->await_object);

		buffer.read(this->waiting_for_alert);
		buffer.read(this->alerted);

		buffer.read_optional(this->await_time);
		buffer.read_optional(this->pending_status);
		buffer.read_optional(this->gs_segment, [this] { return emulator_allocator(*this->emu_ptr); });
		buffer.read_optional(this->teb, [this] { return emulator_object<TEB>(*this->emu_ptr); });

		buffer.read_vector(this->last_registers);
	}

private:
	void setup_registers(x64_emulator& emu, const process_context& context) const;

	void release()
	{
		if (this->marker.was_moved())
		{
			return;
		}

		if (this->stack_base)
		{
			if (!this->emu_ptr)
			{
				throw std::runtime_error("Emulator was never assigned!");
			}

			this->emu_ptr->release_memory(this->stack_base, this->stack_size);
			this->stack_base = 0;
		}

		if (this->gs_segment)
		{
			this->gs_segment->release();
			this->gs_segment = {};
		}
	}
};

struct process_context
{
	process_context(x64_emulator& emu)
		: base_allocator(emu)
		  , peb(emu)
		  , process_params(emu)
		  , kusd(emu, *this)
		  , module_manager(emu)
	{
	}

	registry_manager registry{};

	uint64_t executed_instructions{0};
	uint64_t current_ip{0};
	uint64_t previous_ip{0};

	std::optional<uint64_t> exception_rip{};
	std::optional<NTSTATUS> exit_status{};

	emulator_allocator base_allocator;

	emulator_object<PEB> peb;
	emulator_object<RTL_USER_PROCESS_PARAMETERS> process_params;
	kusd_mmio kusd;

	module_manager module_manager;

	mapped_module* executable{};
	mapped_module* ntdll{};
	mapped_module* win32u{};

	uint64_t ldr_initialize_thunk{};
	uint64_t rtl_user_thread_start{};
	uint64_t ki_user_exception_dispatcher{};

	handle_store<handle_types::event, event> events{};
	handle_store<handle_types::file, file> files{};
	handle_store<handle_types::section, section> sections{};
	handle_store<handle_types::device, io_device_container> devices{};
	handle_store<handle_types::semaphore, semaphore> semaphores{};
	handle_store<handle_types::port, port> ports{};
	handle_store<handle_types::registry, registry_key, 2> registry_keys{};
	std::map<uint16_t, std::wstring> atoms{};

	std::vector<std::byte> default_register_set{};

	uint32_t current_thread_id{0};
	handle_store<handle_types::thread, emulator_thread> threads{};
	emulator_thread* active_thread{nullptr};

	void serialize(utils::buffer_serializer& buffer) const
	{
		buffer.write(this->registry);
		buffer.write(this->executed_instructions);
		buffer.write(this->current_ip);
		buffer.write(this->previous_ip);
		buffer.write_optional(this->exception_rip);
		buffer.write_optional(this->exit_status);
		buffer.write(this->base_allocator);
		buffer.write(this->peb);
		buffer.write(this->process_params);
		buffer.write(this->kusd);
		buffer.write(this->module_manager);

		buffer.write(this->executable->image_base);
		buffer.write(this->ntdll->image_base);
		buffer.write(this->win32u->image_base);

		buffer.write(this->ldr_initialize_thunk);
		buffer.write(this->rtl_user_thread_start);
		buffer.write(this->ki_user_exception_dispatcher);

		buffer.write(this->events);
		buffer.write(this->files);
		buffer.write(this->sections);
		buffer.write(this->devices);
		buffer.write(this->semaphores);
		buffer.write(this->ports);
		buffer.write(this->registry_keys);
		buffer.write_map(this->atoms);

		buffer.write_vector(this->default_register_set);
		buffer.write(this->current_thread_id);
		buffer.write(this->threads);

		buffer.write(this->threads.find_handle(this->active_thread).bits);
	}

	void deserialize(utils::buffer_deserializer& buffer)
	{
		buffer.read(this->registry);
		buffer.read(this->executed_instructions);
		buffer.read(this->current_ip);
		buffer.read(this->previous_ip);
		buffer.read_optional(this->exception_rip);
		buffer.read_optional(this->exit_status);
		buffer.read(this->base_allocator);
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

		buffer.read(this->ldr_initialize_thunk);
		buffer.read(this->rtl_user_thread_start);
		buffer.read(this->ki_user_exception_dispatcher);

		buffer.read(this->events);
		buffer.read(this->files);
		buffer.read(this->sections);
		buffer.read(this->devices);
		buffer.read(this->semaphores);
		buffer.read(this->ports);
		buffer.read(this->registry_keys);
		buffer.read_map(this->atoms);

		buffer.read_vector(this->default_register_set);
		buffer.read(this->current_thread_id);

		buffer.read(this->threads);

		this->active_thread = this->threads.get(buffer.read<uint64_t>());
	}

	handle create_thread(x64_emulator& emu, const uint64_t start_address, const uint64_t argument,
	                     const uint64_t stack_size)
	{
		emulator_thread t{emu, *this, start_address, argument, stack_size, ++this->current_thread_id};
		return this->threads.store(std::move(t));
	}
};
