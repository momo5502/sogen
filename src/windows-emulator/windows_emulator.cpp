#include "std_include.hpp"
#include "windows_emulator.hpp"
#include "context_frame.hpp"

#include <unicorn_x64_emulator.hpp>

constexpr auto MAX_INSTRUCTIONS_PER_TIME_SLICE = 10000;

namespace
{
	template <typename T>
	emulator_object<T> allocate_object_on_stack(x64_emulator& emu)
	{
		const auto old_sp = emu.reg(x64_register::rsp);
		const auto new_sp = align_down(old_sp - sizeof(CONTEXT),
		                               std::max(alignof(CONTEXT), alignof(x64_emulator::pointer_type)));
		emu.reg(x64_register::rsp, new_sp);

		return {emu, new_sp};
	}

	void unalign_stack(x64_emulator& emu)
	{
		auto sp = emu.reg(x64_register::rsp);
		sp = align_down(sp - 0x10, 0x10) + 8;
		emu.reg(x64_register::rsp, sp);
	}

	void setup_stack(x64_emulator& emu, const uint64_t stack_base, const size_t stack_size)
	{
		const uint64_t stack_end = stack_base + stack_size;
		emu.reg(x64_register::rsp, stack_end);
	}

	void setup_gs_segment(x64_emulator& emu, const emulator_allocator& allocator)
	{
		struct msr_value
		{
			uint32_t id;
			uint64_t value;
		};

		const msr_value value{
			IA32_GS_BASE_MSR,
			allocator.get_base()
		};

		emu.write_register(x64_register::msr, &value, sizeof(value));
	}

	emulator_object<KUSER_SHARED_DATA> setup_kusd(x64_emulator& emu)
	{
		emu.allocate_memory(KUSD_ADDRESS, page_align_up(sizeof(KUSER_SHARED_DATA)), memory_permission::read);

		const emulator_object<KUSER_SHARED_DATA> kusd_object{emu, KUSD_ADDRESS};
		kusd_object.access([](KUSER_SHARED_DATA& kusd)
		{
			const auto& real_kusd = *reinterpret_cast<KUSER_SHARED_DATA*>(KUSD_ADDRESS);

			memcpy(&kusd, &real_kusd, sizeof(kusd));

			kusd.ImageNumberLow = IMAGE_FILE_MACHINE_I386;
			kusd.ImageNumberHigh = IMAGE_FILE_MACHINE_AMD64;

			memset(&kusd.ProcessorFeatures, 0, sizeof(kusd.ProcessorFeatures));

			// ...
		});

		return kusd_object;
	}

	uint64_t copy_string(x64_emulator& emu, emulator_allocator& allocator, const void* base_ptr, const uint64_t offset,
	                     const size_t length)
	{
		if (!length)
		{
			return 0;
		}

		const auto length_to_allocate = length + 2;
		const auto str_obj = allocator.reserve(length_to_allocate);
		emu.write_memory(str_obj, static_cast<const uint8_t*>(base_ptr) + offset, length);

		return str_obj;
	}

	ULONG copy_string_as_relative(x64_emulator& emu, emulator_allocator& allocator, const uint64_t result_base,
	                              const void* base_ptr, const uint64_t offset,
	                              const size_t length)
	{
		const auto address = copy_string(emu, allocator, base_ptr, offset, length);
		if (!address)
		{
			return 0;
		}

		assert(address > result_base);
		return static_cast<ULONG>(address - result_base);
	}

	emulator_object<API_SET_NAMESPACE> clone_api_set_map(x64_emulator& emu, emulator_allocator& allocator,
	                                                     const API_SET_NAMESPACE& orig_api_set_map)
	{
		const auto api_set_map_obj = allocator.reserve<API_SET_NAMESPACE>();
		const auto ns_entries_obj = allocator.reserve<API_SET_NAMESPACE_ENTRY>(orig_api_set_map.Count);
		const auto hash_entries_obj = allocator.reserve<API_SET_HASH_ENTRY>(orig_api_set_map.Count);

		api_set_map_obj.access([&](API_SET_NAMESPACE& api_set)
		{
			api_set = orig_api_set_map;
			api_set.EntryOffset = static_cast<ULONG>(ns_entries_obj.value() - api_set_map_obj.value());
			api_set.HashOffset = static_cast<ULONG>(hash_entries_obj.value() - api_set_map_obj.value());
		});

		const auto orig_ns_entries = offset_pointer<API_SET_NAMESPACE_ENTRY>(&orig_api_set_map,
		                                                                     orig_api_set_map.EntryOffset);
		const auto orig_hash_entries = offset_pointer<API_SET_HASH_ENTRY>(&orig_api_set_map,
		                                                                  orig_api_set_map.HashOffset);

		for (ULONG i = 0; i < orig_api_set_map.Count; ++i)
		{
			auto ns_entry = orig_ns_entries[i];
			const auto hash_entry = orig_hash_entries[i];

			ns_entry.NameOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(), &orig_api_set_map,
			                                              ns_entry.NameOffset, ns_entry.NameLength);

			if (!ns_entry.ValueCount)
			{
				continue;
			}

			const auto values_obj = allocator.reserve<API_SET_VALUE_ENTRY>(ns_entry.ValueCount);
			const auto orig_values = offset_pointer<API_SET_VALUE_ENTRY>(&orig_api_set_map,
			                                                             ns_entry.ValueOffset);

			ns_entry.ValueOffset = static_cast<ULONG>(values_obj.value() - api_set_map_obj.value());

			for (ULONG j = 0; j < ns_entry.ValueCount; ++j)
			{
				auto value = orig_values[j];

				value.ValueOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(), &orig_api_set_map,
				                                            value.ValueOffset, value.ValueLength);

				if (value.NameLength)
				{
					value.NameOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(),
					                                           &orig_api_set_map,
					                                           value.NameOffset, value.NameLength);
				}

				values_obj.write(value, j);
			}

			ns_entries_obj.write(ns_entry, i);
			hash_entries_obj.write(hash_entry, i);
		}

		return api_set_map_obj;
	}

	emulator_object<API_SET_NAMESPACE> build_api_set_map(x64_emulator& emu, emulator_allocator& allocator)
	{
		const auto& orig_api_set_map = *NtCurrentTeb()->ProcessEnvironmentBlock->ApiSetMap;
		return clone_api_set_map(emu, allocator, orig_api_set_map);
	}

	emulator_allocator create_allocator(emulator& emu, const size_t size)
	{
		const auto base = emu.find_free_allocation_base(size);
		emu.allocate_memory(base, size, memory_permission::read_write);

		return emulator_allocator{emu, base, size};
	}

	void setup_gdt(x64_emulator& emu)
	{
		constexpr uint64_t gdtr[4] = {0, GDT_ADDR, GDT_LIMIT, 0};
		emu.write_register(x64_register::gdtr, &gdtr, sizeof(gdtr));
		emu.allocate_memory(GDT_ADDR, GDT_LIMIT, memory_permission::read);

		emu.write_memory<uint64_t>(GDT_ADDR + 6 * (sizeof(uint64_t)), 0xEFFE000000FFFF);
		emu.reg<uint16_t>(x64_register::cs, 0x33);

		emu.write_memory<uint64_t>(GDT_ADDR + 5 * (sizeof(uint64_t)), 0xEFF6000000FFFF);
		emu.reg<uint16_t>(x64_register::ss, 0x2B);
	}

	void setup_context(process_context& context, x64_emulator& emu, const std::filesystem::path& file,
	                   const std::vector<std::wstring>& arguments)
	{
		setup_gdt(emu);

		context.kusd = setup_kusd(emu);

		context.base_allocator = create_allocator(emu, PEB_SEGMENT_SIZE);
		auto& allocator = context.base_allocator;

		context.peb = allocator.reserve<PEB>();

		/* Values of the following fields must be
		 * allocated relative to the process_params themselves
		 * and included in the length:
		 *
		 * CurrentDirectory
		 * DllPath
		 * ImagePathName
		 * CommandLine
		 * WindowTitle
		 * DesktopInfo
		 * ShellInfo
		 * RuntimeData
		 * RedirectionDllName
		 */

		context.process_params = allocator.reserve<RTL_USER_PROCESS_PARAMETERS>();

		context.process_params.access([&](RTL_USER_PROCESS_PARAMETERS& proc_params)
		{
			proc_params.Flags = 0x6001; //| 0x80000000; // Prevent CsrClientConnectToServer

			proc_params.ConsoleHandle = CONSOLE_HANDLE.h;
			proc_params.StandardOutput = STDOUT_HANDLE.h;
			proc_params.StandardInput = STDIN_HANDLE.h;
			proc_params.StandardError = proc_params.StandardOutput;


			std::wstring command_line = L"\"" + file.wstring() + L"\"";

			for (const auto& arg : arguments)
			{
				command_line.push_back(L' ');
				command_line.append(arg);
			}

			const auto current_folder = canonical(absolute(file).parent_path()).make_preferred().wstring() + L"\\";

			allocator.make_unicode_string(proc_params.CommandLine, command_line);
			allocator.make_unicode_string(proc_params.CurrentDirectory.DosPath, current_folder);
			allocator.make_unicode_string(proc_params.ImagePathName, file.wstring());

			const auto total_length = allocator.get_next_address() - context.process_params.value();

			proc_params.Length = static_cast<uint32_t>(std::max(sizeof(proc_params), total_length));
			proc_params.MaximumLength = proc_params.Length;
		});

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = nullptr;
			peb.ProcessParameters = context.process_params.ptr();
			peb.ApiSetMap = build_api_set_map(emu, allocator).ptr();

			peb.ProcessHeap = nullptr;
			peb.ProcessHeaps = nullptr;
			peb.HeapSegmentReserve = 0x0000000000100000; // TODO: Read from executable
			peb.HeapSegmentCommit = 0x0000000000002000;
			peb.HeapDeCommitTotalFreeThreshold = 0x0000000000010000;
			peb.HeapDeCommitFreeBlockThreshold = 0x0000000000001000;
			peb.NumberOfHeaps = 0x00000000;
			peb.MaximumNumberOfHeaps = 0x00000010;
		});
	}

	using exception_record_map = std::unordered_map<const EXCEPTION_RECORD*, emulator_object<EXCEPTION_RECORD>>;

	emulator_object<EXCEPTION_RECORD> save_exception_record(emulator_allocator& allocator,
	                                                        const EXCEPTION_RECORD& record,
	                                                        exception_record_map& record_mapping)
	{
		const auto record_obj = allocator.reserve<EXCEPTION_RECORD>();
		record_obj.write(record);

		if (record.ExceptionRecord)
		{
			record_mapping[&record] = record_obj;

			emulator_object<EXCEPTION_RECORD> nested_record_obj{};
			const auto nested_record = record_mapping.find(record.ExceptionRecord);

			if (nested_record != record_mapping.end())
			{
				nested_record_obj = nested_record->second;
			}
			else
			{
				nested_record_obj = save_exception_record(allocator, *record.ExceptionRecord,
				                                          record_mapping);
			}

			record_obj.access([&](EXCEPTION_RECORD& r)
			{
				r.ExceptionRecord = nested_record_obj.ptr();
			});
		}

		return record_obj;
	}

	emulator_object<EXCEPTION_RECORD> save_exception_record(emulator_allocator& allocator,
	                                                        const EXCEPTION_RECORD& record)
	{
		exception_record_map record_mapping{};
		return save_exception_record(allocator, record, record_mapping);
	}

	uint32_t map_violation_operation_to_parameter(const memory_operation operation)
	{
		switch (operation)
		{
		default:
		case memory_operation::read:
			return 0;
		case memory_operation::write:
		case memory_operation::exec:
			return 1;
		}
	}

	size_t calculate_exception_record_size(const EXCEPTION_RECORD& record)
	{
		std::unordered_set<const EXCEPTION_RECORD*> records{};
		size_t total_size = 0;

		const EXCEPTION_RECORD* current_record = &record;
		while (current_record)
		{
			if (!records.insert(current_record).second)
			{
				break;
			}

			total_size += sizeof(*current_record);
			current_record = record.ExceptionRecord;
		}

		return total_size;
	}

	struct machine_frame
	{
		uint64_t rip;
		uint64_t cs;
		uint64_t eflags;
		uint64_t rsp;
		uint64_t ss;
	};

	void dispatch_exception_pointers(x64_emulator& emu, const uint64_t dispatcher, const EXCEPTION_POINTERS pointers)
	{
		constexpr auto mach_frame_size = 0x40;
		constexpr auto context_record_size = 0x4F0;
		const auto exception_record_size = calculate_exception_record_size(*pointers.ExceptionRecord);
		const auto combined_size = align_up(exception_record_size + context_record_size, 0x10);

		assert(combined_size == 0x590);

		const auto allocation_size = combined_size + mach_frame_size;

		const auto initial_sp = emu.reg(x64_register::rsp);
		const auto new_sp = align_down(initial_sp - allocation_size, 0x100);

		const auto total_size = initial_sp - new_sp;
		assert(total_size >= allocation_size);

		std::vector<uint8_t> zero_memory{};
		zero_memory.resize(total_size, 0);

		emu.write_memory(new_sp, zero_memory.data(), zero_memory.size());

		emu.reg(x64_register::rsp, new_sp);
		emu.reg(x64_register::rip, dispatcher);

		const emulator_object<CONTEXT> context_record_obj{emu, new_sp};
		context_record_obj.write(*pointers.ContextRecord);

		emulator_allocator allocator{emu, new_sp + context_record_size, exception_record_size};
		const auto exception_record_obj = save_exception_record(allocator, *pointers.ExceptionRecord);

		if (exception_record_obj.value() != allocator.get_base())
		{
			throw std::runtime_error("Bad exception record position on stack");
		}

		const emulator_object<machine_frame> machine_frame_obj{emu, new_sp + combined_size};
		machine_frame_obj.access([&](machine_frame& frame)
		{
			frame.rip = pointers.ContextRecord->Rip;
			frame.rsp = pointers.ContextRecord->Rsp;
			frame.ss = pointers.ContextRecord->SegSs;
			frame.cs = pointers.ContextRecord->SegCs;
			frame.eflags = pointers.ContextRecord->EFlags;
		});
	}

	void dispatch_access_violation(x64_emulator& emu, const uint64_t dispatcher, const uint64_t address,
	                               const memory_operation operation)
	{
		CONTEXT ctx{};
		ctx.ContextFlags = CONTEXT_ALL;
		context_frame::save(emu, ctx);

		EXCEPTION_RECORD record{};
		memset(&record, 0, sizeof(record));
		record.ExceptionCode = static_cast<DWORD>(STATUS_ACCESS_VIOLATION);
		record.ExceptionFlags = 0;
		record.ExceptionRecord = nullptr;
		record.ExceptionAddress = reinterpret_cast<void*>(emu.read_instruction_pointer());
		record.NumberParameters = 2;
		record.ExceptionInformation[0] = map_violation_operation_to_parameter(operation);
		record.ExceptionInformation[1] = address;

		EXCEPTION_POINTERS pointers{};
		pointers.ContextRecord = &ctx;
		pointers.ExceptionRecord = &record;

		dispatch_exception_pointers(emu, dispatcher, pointers);
	}

	bool switch_to_thread(const logger& logger, x64_emulator& emu, process_context& context, emulator_thread& thread)
	{
		if (thread.exit_status.has_value() || !thread.is_thread_ready(context))
		{
			return false;
		}

		auto* active_thread = context.active_thread;

		if (active_thread == &thread)
		{
			thread.setup_if_necessary(emu, context);
			return true;
		}

		logger.print(color::green, "Performing thread switch...\n");


		if (active_thread)
		{
			active_thread->save(emu);
		}

		context.active_thread = &thread;


		thread.restore(emu);
		thread.setup_if_necessary(emu, context);

		return true;
	}

	bool switch_to_thread(const logger& logger, x64_emulator& emu, process_context& context, const handle thread_handle)
	{
		auto* thread = context.threads.get(thread_handle);
		if (!thread)
		{
			throw std::runtime_error("Bad thread handle");
		}

		return switch_to_thread(logger, emu, context, *thread);
	}

	bool switch_to_next_thread(const logger& logger, x64_emulator& emu, process_context& context)
	{
		bool next_thread = false;

		for (auto& thread : context.threads)
		{
			if (next_thread)
			{
				if (switch_to_thread(logger, emu, context, thread.second))
				{
					return true;
				}

				continue;
			}

			if (&thread.second == context.active_thread)
			{
				next_thread = true;
			}
		}

		for (auto& thread : context.threads)
		{
			if (switch_to_thread(logger, emu, context, thread.second))
			{
				return true;
			}
		}

		return false;
	}

	bool is_object_signaled(process_context& c, const handle h)
	{
		const auto type = h.value.type;

		switch (type)
		{
		default:
			break;

		case handle_types::event:
			{
				const auto* e = c.events.get(h);
				if (e)
				{
					return e->signaled;
				}

				break;
			}

		case handle_types::thread:
			{
				const auto* t = c.threads.get(h);
				if (t)
				{
					return t->exit_status.has_value();
				}

				break;
			}
		}

		throw std::runtime_error("Bad object");
	}
}

emulator_thread::emulator_thread(x64_emulator& emu, const process_context& context,
                                 const uint64_t start_address,
                                 const uint64_t argument,
                                 const uint64_t stack_size, const uint32_t id)
	: emu_ptr(&emu)
	  , stack_size(page_align_up(std::max(stack_size, STACK_SIZE)))
	  , start_address(start_address)
	  , argument(argument)
	  , id(id)
	  , last_registers(context.default_register_set)
{
	this->stack_base = emu.allocate_memory(this->stack_size, memory_permission::read_write);

	this->gs_segment = emulator_allocator{
		emu,
		emu.allocate_memory(GS_SEGMENT_SIZE, memory_permission::read_write),
		GS_SEGMENT_SIZE,
	};

	this->teb = this->gs_segment->reserve<TEB>();

	this->teb->access([&](TEB& teb_obj)
	{
		teb_obj.ClientId.UniqueProcess = reinterpret_cast<HANDLE>(1);
		teb_obj.ClientId.UniqueThread = reinterpret_cast<HANDLE>(static_cast<uint64_t>(this->id));
		teb_obj.NtTib.StackLimit = reinterpret_cast<void*>(this->stack_base);
		teb_obj.NtTib.StackBase = reinterpret_cast<void*>(this->stack_base + this->stack_size);
		teb_obj.NtTib.Self = &this->teb->ptr()->NtTib;
		teb_obj.ProcessEnvironmentBlock = context.peb.ptr();
	});
}

void emulator_thread::mark_as_ready(const NTSTATUS status)
{
	this->pending_status = status;
	this->await_time = {};
	this->await_object = {};

	// TODO: Find out if this is correct
	if (this->waiting_for_alert)
	{
		this->alerted = false;
	}

	this->waiting_for_alert = false;
}

bool emulator_thread::is_thread_ready(process_context& context)
{
	if (this->waiting_for_alert)
	{
		if (this->alerted)
		{
			this->mark_as_ready(STATUS_ALERTED);
			return true;
		}
		if (this->is_await_time_over())
		{
			this->mark_as_ready(STATUS_TIMEOUT);
			return true;
		}

		return false;
	}

	if (this->await_object.has_value())
	{
		if (is_object_signaled(context, *this->await_object))
		{
			this->mark_as_ready(STATUS_WAIT_0);
			return true;
		}

		if (this->is_await_time_over())
		{
			this->mark_as_ready(STATUS_TIMEOUT);
			return true;
		}

		return false;
	}

	if (this->await_time.has_value())
	{
		if (this->is_await_time_over())
		{
			this->mark_as_ready(STATUS_SUCCESS);
			return true;
		}

		return false;
	}

	return true;
}

void emulator_thread::setup_registers(x64_emulator& emu, const process_context& context) const
{
	setup_stack(emu, this->stack_base, this->stack_size);
	setup_gs_segment(emu, *this->gs_segment);

	CONTEXT ctx{};
	ctx.ContextFlags = CONTEXT_ALL;

	unalign_stack(emu);
	context_frame::save(emu, ctx);

	ctx.Rip = context.rtl_user_thread_start;
	ctx.Rcx = this->start_address;
	ctx.Rdx = this->argument;

	const auto ctx_obj = allocate_object_on_stack<CONTEXT>(emu);
	ctx_obj.write(ctx);

	unalign_stack(emu);

	emu.reg(x64_register::rcx, ctx_obj.value());
	emu.reg(x64_register::rdx, context.ntdll->image_base);
	emu.reg(x64_register::rip, context.ldr_initialize_thunk);
}

std::unique_ptr<x64_emulator> create_default_x64_emulator()
{
	return unicorn::create_x64_emulator();
}

windows_emulator::windows_emulator(const std::filesystem::path& application, const std::vector<std::wstring>& arguments,
                                   std::unique_ptr<x64_emulator> emu)
	: windows_emulator(std::move(emu))
{
	this->setup_process(application, arguments);
}

windows_emulator::windows_emulator(std::unique_ptr<x64_emulator> emu)
	: emu_(std::move(emu))
	  , process_(*emu_)
{
	this->setup_hooks();
}

void windows_emulator::setup_process(const std::filesystem::path& application,
                                     const std::vector<std::wstring>& arguments)
{
	auto& emu = this->emu();

	auto& context = this->process();
	context.module_manager = module_manager(emu); // TODO: Cleanup module manager

	setup_context(context, emu, application, arguments);

	context.executable = context.module_manager.map_module(application, this->logger);

	context.peb.access([&](PEB& peb)
	{
		peb.ImageBaseAddress = reinterpret_cast<void*>(context.executable->image_base);
	});

	context.ntdll = context.module_manager.map_module(R"(C:\Windows\System32\ntdll.dll)", this->logger);
	context.win32u = context.module_manager.map_module(R"(C:\Windows\System32\win32u.dll)", this->logger);

	this->dispatcher_.setup(context.ntdll->exports, context.win32u->exports);

	context.ldr_initialize_thunk = context.ntdll->find_export("LdrInitializeThunk");
	context.rtl_user_thread_start = context.ntdll->find_export("RtlUserThreadStart");
	context.ki_user_exception_dispatcher = context.ntdll->find_export("KiUserExceptionDispatcher");

	context.default_register_set = emu.save_registers();

	const auto main_thread_id = context.create_thread(emu, context.executable->entry_point, 0, 0);
	switch_to_thread(this->logger, emu, context, main_thread_id);
}

void windows_emulator::perform_thread_switch()
{
	this->switch_thread = false;
	while (!switch_to_next_thread(this->logger, this->emu(), this->process()))
	{
		// TODO: Optimize that
		std::this_thread::sleep_for(1ms);
	}
}

void windows_emulator::setup_hooks()
{
	this->emu().hook_instruction(x64_hookable_instructions::syscall, [&]
	{
		for (const auto& hook : this->syscall_hooks_)
		{
			if (hook() == instruction_hook_continuation::skip_instruction)
			{
				return instruction_hook_continuation::skip_instruction;
			}
		}

		this->dispatcher_.dispatch(*this);
		return instruction_hook_continuation::skip_instruction;
	});

	this->emu().hook_instruction(x64_hookable_instructions::invalid, [&]
	{
		const auto ip = this->emu().read_instruction_pointer();
		printf("Invalid instruction at: 0x%llX\n", ip);
		return instruction_hook_continuation::skip_instruction;
	});

	this->emu().hook_interrupt([&](const int interrupt)
	{
		const auto rip = this->emu().read_instruction_pointer();
		printf("Interrupt: %i 0x%llX\n", interrupt, rip);

		if (this->fuzzing)
		{
			this->process().exception_rip = rip;
			this->emu().stop();
		}
	});

	this->emu().hook_memory_violation([&](const uint64_t address, const size_t size, const memory_operation operation,
	                                      const memory_violation_type type)
	{
		const auto permission = get_permission_string(operation);
		const auto ip = this->emu().read_instruction_pointer();
		const char* name = this->process().module_manager.find_name(ip);

		if (type == memory_violation_type::protection)
		{
			printf("Protection violation: 0x%llX (%zX) - %s at 0x%llX (%s)\n", address, size, permission.c_str(), ip,
			       name);
		}
		else if (type == memory_violation_type::unmapped)
		{
			printf("Mapping violation: 0x%llX (%zX) - %s at 0x%llX (%s)\n", address, size, permission.c_str(), ip,
			       name);
		}

		if (this->fuzzing)
		{
			this->process().exception_rip = ip;
			this->emu().stop();
			return memory_violation_continuation::stop;
		}

		dispatch_access_violation(this->emu(), this->process().ki_user_exception_dispatcher, address, operation);
		return memory_violation_continuation::resume;
	});

	this->emu().hook_memory_execution(0, std::numeric_limits<size_t>::max(),
	                                  [&](const uint64_t address, const size_t, const uint64_t)
	                                  {
		                                  auto& process = this->process();
		                                  auto& thread = this->current_thread();

		                                  ++process.executed_instructions;
		                                  const auto thread_insts = ++thread.executed_instructions;
		                                  if (thread_insts % MAX_INSTRUCTIONS_PER_TIME_SLICE == 0)
		                                  {
			                                  this->switch_thread = true;
			                                  this->emu().stop();
		                                  }

		                                  process.previous_ip = process.current_ip;
		                                  process.current_ip = this->emu().read_instruction_pointer();

		                                  const auto is_interesting_call = process.executable->is_within(
			                                  process.previous_ip) || process.executable->is_within(address);

		                                  /*if (address == 0x180038B65)
		                                  {
			                                  puts("!!! DLL init failed");
		                                  }
		                                  if (address == 0x180038A20)
		                                  {
			                                  const auto* name = this->process().module_manager.find_name(
				                                  this->emu().reg(x64_register::rcx));
			                                  printf("!!! DLL init: %s\n", name);
		                                  }*/

		                                  if (!this->verbose && !this->verbose_calls && !is_interesting_call)
		                                  {
			                                  return;
		                                  }

		                                  const auto* binary = this->process().module_manager.find_by_address(address);

		                                  if (binary)
		                                  {
			                                  const auto export_entry = binary->address_names.find(address);
			                                  if (export_entry != binary->address_names.end())
			                                  {
				                                  logger.print(is_interesting_call ? color::yellow : color::dark_gray,
				                                               "Executing function: %s - %s (0x%llX)\n",
				                                               binary->name.c_str(),
				                                               export_entry->second.c_str(), address);
			                                  }
			                                  else if (address == binary->entry_point)
			                                  {
				                                  logger.print(is_interesting_call ? color::yellow : color::gray,
				                                               "Executing entry point: %s (0x%llX)\n",
				                                               binary->name.c_str(),
				                                               address);
			                                  }
		                                  }

		                                  if (!this->verbose)
		                                  {
			                                  return;
		                                  }

		                                  auto& emu = this->emu();

		                                  printf(
			                                  "Inst: %16llX - RAX: %16llX - RBX: %16llX - RCX: %16llX - RDX: %16llX - R8: %16llX - R9: %16llX - RDI: %16llX - RSI: %16llX - %s\n",
			                                  address,
			                                  emu.reg(x64_register::rax), emu.reg(x64_register::rbx),
			                                  emu.reg(x64_register::rcx),
			                                  emu.reg(x64_register::rdx), emu.reg(x64_register::r8),
			                                  emu.reg(x64_register::r9),
			                                  emu.reg(x64_register::rdi), emu.reg(x64_register::rsi),
			                                  binary ? binary->name.c_str() : "<N/A>");
	                                  });
}

void windows_emulator::serialize(utils::buffer_serializer& buffer) const
{
	this->emu().serialize(buffer);
	this->process_.serialize(buffer);
	this->dispatcher_.serialize(buffer);
}

void windows_emulator::deserialize(utils::buffer_deserializer& buffer)
{
	this->emu().deserialize(buffer);
	this->process_.deserialize(buffer);
	this->dispatcher_.deserialize(buffer);
}

void windows_emulator::save_snapshot()
{
	this->emu().save_snapshot();

	utils::buffer_serializer serializer{};
	this->process_.serialize(serializer);

	this->process_snapshot_ = serializer.move_buffer();

	// TODO: Make process copyable
	//this->process_snapshot_ = this->process();
}

void windows_emulator::restore_snapshot()
{
	if (this->process_snapshot_.empty())
	{
		assert(false);
		return;
	}

	this->emu().restore_snapshot();

	utils::buffer_deserializer deserializer{this->process_snapshot_};
	this->process_.deserialize(deserializer);
	//this->process_ = *this->process_snapshot_;
}
