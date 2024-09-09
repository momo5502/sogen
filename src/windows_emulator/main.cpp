#include <gdbstub.h>

#include "std_include.hpp"

#include "emulator_utils.hpp"
#include "process_context.hpp"
#include "syscalls.hpp"

#include "reflect_extension.hpp"
#include <reflect>

#include <address_utils.hpp>
#include <unicorn_x64_emulator.hpp>

#include "context_frame.hpp"

#include "debugging/x64_gdb_stub_handler.hpp"


#define GS_SEGMENT_ADDR 0x6000000ULL
#define GS_SEGMENT_SIZE (20 << 20)  // 20 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_SIZE 0x40000
#define STACK_ADDRESS (0x80000000000 - STACK_SIZE)
#define KUSD_ADDRESS 0x7ffe0000

#define GDT_ADDR 0x30000
#define GDT_LIMIT 0x1000
#define GDT_ENTRY_SIZE 0x8

bool use_gdb = false;

namespace
{
	template <typename T>
	class type_info
	{
	public:
		type_info()
		{
			this->type_name_ = reflect::type_name<T>();

			reflect::for_each<T>([this](auto I)
			{
				const auto member_name = reflect::member_name<I, T>();
				const auto member_offset = reflect::offset_of<I, T>();

				this->members_[member_offset] = member_name;
			});
		}

		std::string get_member_name(const size_t offset) const
		{
			size_t last_offset{};
			std::string_view last_member{};

			for (const auto& member : this->members_)
			{
				if (offset == member.first)
				{
					return member.second;
				}

				if (offset < member.first)
				{
					const auto diff = offset - last_offset;
					return std::string(last_member) + "+" + std::to_string(diff);
				}

				last_offset = member.first;
				last_member = member.second;
			}

			return "<N/A>";
		}

		const std::string& get_type_name() const
		{
			return this->type_name_;
		}

	private:
		std::string type_name_{};
		std::map<size_t, std::string> members_{};
	};

	template <typename T>
	void watch_object(x64_emulator& emu, emulator_object<T> object)
	{
		const type_info<T> info{};

		emu.hook_memory_read(object.value(), object.size(),
		                     [i = std::move(info), object](const uint64_t address, size_t)
		                     {
			                     const auto offset = address - object.value();
			                     printf("%s: %llX (%s)\n", i.get_type_name().c_str(), offset,
			                            i.get_member_name(offset).c_str());
		                     });
	}

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
		emu.allocate_memory(stack_base, stack_size, memory_permission::read_write);

		const uint64_t stack_end = stack_base + stack_size;
		emu.reg(x64_register::rsp, stack_end);
	}

	emulator_allocator setup_gs_segment(x64_emulator& emu, const uint64_t segment_base, const uint64_t size)
	{
		struct msr_value
		{
			uint32_t id;
			uint64_t value;
		};

		const msr_value value{
			IA32_GS_BASE_MSR,
			segment_base
		};

		emu.write_register(x64_register::msr, &value, sizeof(value));
		emu.allocate_memory(segment_base, size, memory_permission::read_write);

		return {emu, segment_base, size};
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

		//watch_object(emu, api_set_map_obj);

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

	process_context setup_context(x64_emulator& emu)
	{
		process_context context{};

		setup_stack(emu, STACK_ADDRESS, STACK_SIZE);
		setup_gdt(emu);

		context.kusd = setup_kusd(emu);
		context.gs_segment = setup_gs_segment(emu, GS_SEGMENT_ADDR, GS_SEGMENT_SIZE);

		auto allocator = create_allocator(emu, 1 << 20);

		auto& gs = context.gs_segment;

		context.teb = gs.reserve<TEB>();
		context.peb = gs.reserve<PEB>();
		context.process_params = gs.reserve<RTL_USER_PROCESS_PARAMETERS>();

		context.teb.access([&](TEB& teb)
		{
			teb.ClientId.UniqueProcess = reinterpret_cast<HANDLE>(1);
			teb.ClientId.UniqueThread = reinterpret_cast<HANDLE>(2);
			teb.NtTib.StackLimit = reinterpret_cast<void*>(STACK_ADDRESS);
			teb.NtTib.StackBase = reinterpret_cast<void*>((STACK_ADDRESS + STACK_SIZE));
			teb.NtTib.Self = &context.teb.ptr()->NtTib;
			teb.ProcessEnvironmentBlock = context.peb.ptr();
		});

		context.process_params.access([&](RTL_USER_PROCESS_PARAMETERS& proc_params)
		{
			proc_params.Length = sizeof(proc_params);
			proc_params.Flags = 0x6001 | 0x80000000; // Prevent CsrClientConnectToServer

			proc_params.ConsoleHandle = CONSOLE_HANDLE.h;
			proc_params.StandardOutput = STDOUT_HANDLE.h;
			proc_params.StandardInput = STDIN_HANDLE.h;
			proc_params.StandardError = proc_params.StandardOutput;

			gs.make_unicode_string(proc_params.CurrentDirectory.DosPath, L"C:\\Users\\mauri\\Desktop");
			gs.make_unicode_string(proc_params.ImagePathName, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
			gs.make_unicode_string(proc_params.CommandLine, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
		});

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = nullptr;
			peb.ProcessHeap = nullptr;
			peb.ProcessHeaps = nullptr;
			peb.ProcessParameters = context.process_params.ptr();
			peb.ApiSetMap = build_api_set_map(emu, allocator).ptr();
		});

		return context;
	}

	uint64_t find_exported_function(const std::vector<exported_symbol>& exports, const std::string_view name)
	{
		for (auto& symbol : exports)
		{
			if (symbol.name == name)
			{
				return symbol.address;
			}
		}

		return 0;
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
			return 1;
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

		printf("ContextRecord: %llX\n", context_record_obj.value());
		printf("ExceptionRecord: %llX\n", exception_record_obj.value());
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

	void run()
	{
		const auto emu = unicorn::create_x64_emulator();

		auto context = setup_context(*emu);
		context.module_manager = module_manager(*emu);

		context.executable = context.module_manager.map_module(R"(C:\Users\mauri\Desktop\ConsoleApplication6.exe)");

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = reinterpret_cast<void*>(context.executable->image_base);
		});

		context.ntdll = context.module_manager.map_module(R"(C:\Windows\System32\ntdll.dll)");

		const auto ldr_initialize_thunk = find_exported_function(context.ntdll->exports, "LdrInitializeThunk");
		const auto rtl_user_thread_start = find_exported_function(context.ntdll->exports, "RtlUserThreadStart");
		const auto ki_user_exception_dispatcher = find_exported_function(
			context.ntdll->exports, "KiUserExceptionDispatcher");

		syscall_dispatcher dispatcher{context.ntdll->exports};

		emu->hook_instruction(x64_hookable_instructions::syscall, [&]
		{
			dispatcher.dispatch(*emu, context);
			return instruction_hook_continuation::skip_instruction;
		});

		emu->hook_instruction(x64_hookable_instructions::invalid, [&]
		{
			const auto ip = emu->read_instruction_pointer();
			printf("Invalid instruction at: %llX\n", ip);
			return instruction_hook_continuation::skip_instruction;
		});

		emu->hook_interrupt([&](int interrupt)
		{
			printf("Interrupt: %i %llX\n", interrupt, emu->read_instruction_pointer());
		});

		emu->hook_memory_violation([&](const uint64_t address, const size_t size, const memory_operation operation,
		                               const memory_violation_type type)
		{
			const auto permission = get_permission_string(operation);
			const auto ip = emu->read_instruction_pointer();

			if (type == memory_violation_type::protection)
			{
				printf("Protection violation: %llX (%zX) - %s at %llX\n", address, size, permission.c_str(), ip);
			}
			else if (type == memory_violation_type::unmapped)
			{
				printf("Mapping violation: %llX (%zX) - %s at %llX\n", address, size, permission.c_str(), ip);
			}

			dispatch_access_violation(*emu, ki_user_exception_dispatcher, address, operation);
			return memory_violation_continuation::resume;
		});

		watch_object(*emu, context.teb);
		watch_object(*emu, context.peb);
		watch_object(*emu, context.process_params);
		watch_object(*emu, context.kusd);

		context.verbose = false;

		emu->hook_memory_execution(0, std::numeric_limits<size_t>::max(), [&](const uint64_t address, const size_t)
		{
			++context.executed_instructions;

			const auto* binary = context.module_manager.find_by_address(address);

			if (binary)
			{
				const auto export_entry = binary->address_names.find(address);
				if (export_entry != binary->address_names.end())
				{
					printf("Executing function: %s - %s (%llX)\n", binary->name.c_str(), export_entry->second.c_str(),
					       address);
				}
			}

			if (!context.verbose)
			{
				return;
			}

			printf(
				"Inst: %16llX - RAX: %16llX - RBX: %16llX - RCX: %16llX - RDX: %16llX - R8: %16llX - R9: %16llX - RDI: %16llX - RSI: %16llX\n",
				address,
				emu->reg(x64_register::rax), emu->reg(x64_register::rbx), emu->reg(x64_register::rcx),
				emu->reg(x64_register::rdx), emu->reg(x64_register::r8), emu->reg(x64_register::r9),
				emu->reg(x64_register::rdi), emu->reg(x64_register::rsi));
		});

		CONTEXT ctx{};
		ctx.ContextFlags = CONTEXT_ALL;

		unalign_stack(*emu);

		context_frame::save(*emu, ctx);

		ctx.Rip = rtl_user_thread_start;
		ctx.Rcx = context.executable->entry_point;

		const auto ctx_obj = allocate_object_on_stack<CONTEXT>(*emu);
		ctx_obj.write(ctx);

		unalign_stack(*emu);

		emu->reg(x64_register::rcx, ctx_obj.value());
		emu->reg(x64_register::rdx, context.ntdll->image_base);
		emu->reg(x64_register::rip, ldr_initialize_thunk);

		try
		{
			if (use_gdb)
			{
				puts("Launching gdb stub...");

				x64_gdb_stub_handler handler{*emu};
				run_gdb_stub(handler, "i386:x86-64", gdb_registers.size(), "0.0.0.0:28960");
			}
			else
			{
				emu->start_from_ip();
			}
		}
		catch (...)
		{
			printf("Emulation failed at: %llX\n", emu->reg(x64_register::rip));
			throw;
		}

		printf("Emulation done.\n");
	}
}

int main(int /*argc*/, char** /*argv*/)
{
	//setvbuf(stdout, nullptr, _IOFBF, 0x10000);

	try
	{
		do
		{
			run();
		}
		while (use_gdb);

		return 0;
	}
	catch (std::exception& e)
	{
		puts(e.what());

#ifdef _WIN32
		//MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
#endif
	}

	return 1;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
	return main(__argc, __argv);
}
#endif
