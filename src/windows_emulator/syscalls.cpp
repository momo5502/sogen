#include "std_include.hpp"
#include "syscalls.hpp"

namespace
{
	struct syscall_context
	{
		x64_emulator& emu;
		process_context& proc;
	};

	uint64_t get_syscall_argument(x64_emulator& emu, const size_t index)
	{
		switch (index)
		{
		case 0:
			return emu.reg(x64_register::r10);
		case 1:
			return emu.reg(x64_register::rdx);
		case 2:
			return emu.reg(x64_register::r8);
		case 3:
			return emu.reg(x64_register::r9);
		default:
			return emu.read_stack(index + 1);
		}
	}

	template <typename T>
		requires(std::is_integral_v<T>)
	T resolve_argument(x64_emulator& emu, const size_t index)
	{
		const auto arg = get_syscall_argument(emu, index);
		return static_cast<T>(arg);
	}

	template <typename T>
		requires(std::is_same_v<T, emulator_object<typename T::value_type>>)
	T resolve_argument(x64_emulator& emu, const size_t index)
	{
		const auto arg = get_syscall_argument(emu, index);
		return T(emu, arg);
	}

	template <typename T>
	T resolve_indexed_argument(x64_emulator& emu, size_t& index)
	{
		return resolve_argument<T>(emu, index++);
	}

	void forward(const syscall_context& c, NTSTATUS (*handler)())
	{
		const auto ret = handler();
		c.emu.reg<uint64_t>(x64_register::rax, static_cast<uint64_t>(ret));
	}

	template <typename... Args>
	void forward(const syscall_context& c, NTSTATUS (*handler)(const syscall_context&, Args...))
	{
		size_t index = 0;
		std::tuple<const syscall_context&, Args...> func_args
		{
			c,
			resolve_indexed_argument<std::remove_cv_t<std::remove_reference_t<Args>>>(c.emu, index)...
		};

		const auto ret = std::apply(handler, std::move(func_args));
		c.emu.reg<uint64_t>(x64_register::rax, static_cast<uint64_t>(ret));
	}

	void handle_NtQueryPerformanceCounter(x64_emulator& emu)
	{
		const emulator_object<LARGE_INTEGER> performance_counter{emu, emu.reg(x64_register::r10)};
		const emulator_object<LARGE_INTEGER> performance_frequency{emu, emu.reg(x64_register::rdx)};

		try
		{
			if (performance_counter)
			{
				performance_counter.access([](LARGE_INTEGER& value)
				{
					QueryPerformanceCounter(&value);
				});
			}

			if (performance_frequency)
			{
				performance_frequency.access([](LARGE_INTEGER& value)
				{
					QueryPerformanceFrequency(&value);
				});
			}

			emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
		}
		catch (...)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_ACCESS_VIOLATION);
		}
	}

	NTSTATUS handle_NtManageHotPatch()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtOpenKey()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtCreateIoCompletion()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtTraceEvent()
	{
		return STATUS_NOT_SUPPORTED;
	}

	void handle_NtCreateEvent(x64_emulator& emu, process_context& context)
	{
		const emulator_object<uint64_t> event_handle{emu, emu.reg(x64_register::r10)};
		const auto object_attributes = emu.reg(x64_register::r8);
		const auto event_type = emu.reg<EVENT_TYPE>(x64_register::r9d);
		const auto initial_state = static_cast<BOOLEAN>(emu.read_stack(5));

		if (object_attributes)
		{
			puts("Unsupported object attributes");
			emu.stop();
			return;
		}

		const uint64_t index = context.events.size();
		event_handle.write(index);

		context.events.emplace_back(initial_state != FALSE, event_type);

		static_assert(sizeof(EVENT_TYPE) == sizeof(uint32_t));

		emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
	}

	void handle_NtQueryVirtualMemory(x64_emulator& emu, const process_context& context)
	{
		const auto process_handle = emu.reg(x64_register::r10);
		const auto base_address = emu.reg(x64_register::rdx);
		const auto info_class = emu.reg<uint32_t>(x64_register::r8d);
		const auto memory_information = emu.reg(x64_register::r9);
		const auto memory_information_length = static_cast<uint32_t>(emu.read_stack(5));
		const emulator_object<uint32_t> return_length{emu, emu.read_stack(6)};

		if (process_handle != ~0ULL)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			return;
		}

		if (info_class == MemoryWorkingSetExInformation)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			return;
		}

		if (info_class != MemoryImageInformation)
		{
			printf("Unsupported memory info class: %X\n", info_class);
			emu.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(MEMORY_IMAGE_INFORMATION));
		}

		if (memory_information_length != sizeof(MEMORY_IMAGE_INFORMATION))
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_BUFFER_OVERFLOW);
			return;
		}

		if (!is_within_start_and_length(base_address, context.ntdll.image_base, context.ntdll.size_of_image))
		{
			puts("Bad image request");
			emu.stop();
			return;
		}

		const emulator_object<MEMORY_IMAGE_INFORMATION> info{emu, memory_information};

		info.access([&](MEMORY_IMAGE_INFORMATION& image_info)
		{
			image_info.ImageBase = reinterpret_cast<void*>(context.ntdll.image_base);
			image_info.SizeOfImage = context.ntdll.size_of_image;
		});

		emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
	}

	void handle_NtQuerySystemInformation(x64_emulator& emu)
	{
		const auto info_class = emu.reg<uint32_t>(x64_register::r10d);
		const auto system_information = emu.reg(x64_register::rdx);
		const auto system_information_length = emu.reg<uint32_t>(x64_register::r8d);
		const emulator_object<uint32_t> return_length{emu, emu.reg(x64_register::r9)};

		if (info_class == SystemFlushInformation
			|| info_class == SystemHypervisorSharedPageInformation)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_SUPPORTED);
			return;
		}

		if (info_class == SystemNumaProcessorMap)
		{
			if (return_length)
			{
				return_length.write(sizeof(SYSTEM_NUMA_INFORMATION));
			}

			if (system_information_length != sizeof(SYSTEM_NUMA_INFORMATION))
			{
				emu.reg<uint64_t>(x64_register::rax, STATUS_BUFFER_TOO_SMALL);
				return;
			}

			const emulator_object<SYSTEM_NUMA_INFORMATION> info_obj{emu, system_information};

			info_obj.access([&](SYSTEM_NUMA_INFORMATION& info)
			{
				memset(&info, 0, sizeof(info));
				info.ActiveProcessorsGroupAffinity->Mask = 0xFFF;
				info.AvailableMemory[0] = 0xFFF;
				info.Pad[0] = 0xFFF;
			});

			emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
			return;
		}

		if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
		{
			printf("Unsupported system info class: %X\n", info_class);
			emu.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(SYSTEM_BASIC_INFORMATION));
		}

		if (system_information_length != sizeof(SYSTEM_BASIC_INFORMATION))
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_BUFFER_TOO_SMALL);
			return;
		}

		const emulator_object<SYSTEM_BASIC_INFORMATION> info{emu, system_information};

		info.access([&](SYSTEM_BASIC_INFORMATION& basic_info)
		{
			basic_info.Reserved = 0;
			basic_info.TimerResolution = 0x0002625a;
			basic_info.PageSize = 0x1000;
			basic_info.LowestPhysicalPageNumber = 0x00000001;
			basic_info.HighestPhysicalPageNumber = 0x00c9c7ff;
			basic_info.AllocationGranularity = 0x10000;
			basic_info.MinimumUserModeAddress = 0x0000000000010000;
			basic_info.MaximumUserModeAddress = 0x00007ffffffeffff;
			basic_info.ActiveProcessorsAffinityMask = 0x0000000000000fff;
			basic_info.NumberOfProcessors = 1;
		});

		emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
	}

	void handle_NtQuerySystemInformationEx(x64_emulator& emu)
	{
		const auto info_class = emu.reg<uint32_t>(x64_register::r10d);
		const auto input_buffer = emu.reg(x64_register::rdx);
		const auto input_buffer_length = emu.reg<uint32_t>(x64_register::r8d);
		const auto system_information = emu.reg(x64_register::r9);
		const auto system_information_length = static_cast<uint32_t>(emu.read_stack(5));
		const emulator_object<uint32_t> return_length{emu, emu.read_stack(6)};

		if (info_class == SystemFlushInformation
			|| info_class == SystemFeatureConfigurationInformation
			|| info_class == SystemFeatureConfigurationSectionInformation)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_SUPPORTED);
			return;
		}

		if (info_class == SystemLogicalProcessorAndGroupInformation)
		{
			void* buffer = calloc(1, input_buffer_length);
			void* res_buff = calloc(1, system_information_length);
			emu.read_memory(input_buffer, buffer, input_buffer_length);

			uint64_t code = 0;

			return_length.access([&](uint32_t& len)
			{
				code = NtQuerySystemInformationEx((SYSTEM_INFORMATION_CLASS)info_class, buffer, input_buffer_length,
				                                  res_buff,
				                                  system_information_length, (ULONG*)&len);
			});

			if (code == 0)
			{
				emu.write_memory(system_information, res_buff, return_length.read());
			}

			free(buffer);
			free(res_buff);

			emu.reg<uint64_t>(x64_register::rax, code);
			return;
		}

		if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
		{
			printf("Unsupported system info ex class: %X\n", info_class);
			emu.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(SYSTEM_BASIC_INFORMATION));
		}

		if (system_information_length != sizeof(SYSTEM_BASIC_INFORMATION))
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_BUFFER_TOO_SMALL);
			return;
		}

		const emulator_object<SYSTEM_BASIC_INFORMATION> info{emu, system_information};

		info.access([&](SYSTEM_BASIC_INFORMATION& basic_info)
		{
			basic_info.Reserved = 0;
			basic_info.TimerResolution = 0x0002625a;
			basic_info.PageSize = 0x1000;
			basic_info.LowestPhysicalPageNumber = 0x00000001;
			basic_info.HighestPhysicalPageNumber = 0x00c9c7ff;
			basic_info.AllocationGranularity = 0x10000;
			basic_info.MinimumUserModeAddress = 0x0000000000010000;
			basic_info.MaximumUserModeAddress = 0x00007ffffffeffff;
			basic_info.ActiveProcessorsAffinityMask = 0x0000000000000fff;
			basic_info.NumberOfProcessors = 1;
		});

		emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
	}

	void handle_NtQueryProcessInformation(x64_emulator& emu)
	{
		const auto process_handle = emu.reg<uint64_t>(x64_register::r10);
		const auto info_class = emu.reg<uint32_t>(x64_register::edx);
		const auto process_information = emu.reg(x64_register::r8);
		const auto process_information_length = emu.reg<uint32_t>(x64_register::r9d);
		const emulator_object<uint32_t> return_length{emu, emu.read_stack(5)};

		if (process_handle != ~0ULL)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			return;
		}

		if (info_class != ProcessCookie)
		{
			printf("Unsupported process info class: %X\n", info_class);
			emu.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(uint32_t));
		}

		if (process_information_length != sizeof(uint32_t))
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_BUFFER_OVERFLOW);
			return;
		}

		const emulator_object<uint32_t> info{emu, process_information};
		info.write(0x01234567);

		emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
	}

	void handle_NtProtectVirtualMemory(x64_emulator& emu)
	{
		const auto process_handle = emu.reg(x64_register::r10);
		const emulator_object<uint64_t> base_address{emu, emu.reg(x64_register::rdx)};
		const emulator_object<uint32_t> bytes_to_protect{emu, emu.reg(x64_register::r8)};
		const auto protection = emu.reg<uint32_t>(x64_register::r9d);
		const emulator_object<uint32_t> old_protection{emu, emu.read_stack(5)};

		if (process_handle != ~0ULL)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			return;
		}

		const auto address = page_align_down(base_address.read());
		base_address.write(address);

		const auto size = page_align_up(bytes_to_protect.read());
		bytes_to_protect.write(static_cast<uint32_t>(size));

		const auto current_uc_protection = get_memory_protection(emu, address);
		const auto current_protection = map_emulator_to_nt_protection(current_uc_protection);
		old_protection.write(current_protection);

		const auto requested_protection = map_nt_to_emulator_protection(protection);
		emu.protect_memory(address, size, requested_protection);

		emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
	}

	NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, const uint64_t process_handle,
	                                        const emulator_object<uint64_t> base_address,
	                                        const emulator_object<uint64_t> bytes_to_allocate,
	                                        const uint32_t /*allocation_type*/, const uint32_t page_protection)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_IMPLEMENTED;
		}

		constexpr auto allocation_granularity = 0x10000;
		const auto allocation_bytes = bytes_to_allocate.read();
		//allocation_bytes = align_up(allocation_bytes, allocation_granularity);
		//bytes_to_allocate.write(allocation_bytes);

		const auto protection = map_nt_to_emulator_protection(page_protection);

		auto allocate_anywhere = false;
		auto allocation_base = base_address.read();
		if (!allocation_base)
		{
			allocate_anywhere = true;
			allocation_base = allocation_granularity;
		}
		else if (is_memory_allocated(c.emu, allocation_base))
		{
			return STATUS_SUCCESS;
		}

		bool succeeded = false;

		while (true)
		{
			succeeded = c.emu.try_map_memory(allocation_base, allocation_bytes, protection);
			if (succeeded || !allocate_anywhere)
			{
				break;
			}

			allocation_base += allocation_granularity;
		}

		base_address.write(allocation_base);

		return succeeded
			       ? STATUS_SUCCESS
			       : STATUS_NOT_SUPPORTED; // No idea what the correct code is
	}

	void handle_NtAllocateVirtualMemoryEx(x64_emulator& emu)
	{
		const auto process_handle = emu.reg(x64_register::r10);
		const emulator_object<uint64_t> base_address{emu, emu.reg(x64_register::rdx)};
		const emulator_object<uint64_t> bytes_to_allocate{emu, emu.reg(x64_register::r8)};
		//const auto allocation_type =emu.reg<uint32_t>(x64_register::r9d);
		const auto page_protection = static_cast<uint32_t>(emu.read_stack(5));

		if (process_handle != ~0ULL)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			return;
		}

		constexpr auto allocation_granularity = 0x10000;
		const auto allocation_bytes = bytes_to_allocate.read();
		//allocation_bytes = align_up(allocation_bytes, allocation_granularity);
		//bytes_to_allocate.write(allocation_bytes);

		const auto protection = map_nt_to_emulator_protection(page_protection);

		auto allocate_anywhere = false;
		auto allocation_base = base_address.read();
		if (!allocation_base)
		{
			allocate_anywhere = true;
			allocation_base = allocation_granularity;
		}
		else if (is_memory_allocated(emu, allocation_base))
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_SUCCESS);
			return;
		}

		bool succeeded = false;

		while (true)
		{
			succeeded = emu.try_map_memory(allocation_base, allocation_bytes, protection);
			if (succeeded || !allocate_anywhere)
			{
				break;
			}

			allocation_base += allocation_granularity;
		}

		base_address.write(allocation_base);

		emu.reg<uint64_t>(x64_register::rax, succeeded
			                                     ? STATUS_SUCCESS
			                                     : STATUS_NOT_SUPPORTED // No idea what the correct code is
		);
	}

	void handle_NtFreeVirtualMemory(x64_emulator& emu)
	{
		const auto process_handle = emu.reg(x64_register::r10);
		const emulator_object<uint64_t> base_address{emu, emu.reg(x64_register::rdx)};
		const emulator_object<uint64_t> bytes_to_allocate{emu, emu.reg(x64_register::r8)};

		if (process_handle != ~0ULL)
		{
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			return;
		}

		const auto allocation_base = base_address.read();
		const auto allocation_size = bytes_to_allocate.read();

		bool succeeded = false;
		try
		{
			emu.unmap_memory(allocation_base, allocation_size);
			succeeded = true;
		}
		catch (...)
		{
			succeeded = false;
		}

		emu.reg<uint64_t>(x64_register::rax, succeeded
			                                     ? STATUS_SUCCESS
			                                     : STATUS_NOT_SUPPORTED // No idea what the correct code is
		);
	}
}

void handle_syscall(x64_emulator& emu, process_context& context)
{
	const auto address = emu.read_instruction_pointer();
	const auto syscall_id = emu.reg<uint32_t>(x64_register::eax);

	printf("Handling syscall: %X (%llX)\n", syscall_id, address);

	syscall_context c{emu, context};

	try
	{
		switch (syscall_id)
		{
		case 0x12:
			forward(c, handle_NtOpenKey);
			break;
		case 0x18:
			forward(c, handle_NtAllocateVirtualMemory);
			break;
		case 0x1E:
			handle_NtFreeVirtualMemory(emu);
			break;
		case 0x19:
			handle_NtQueryProcessInformation(emu);
			break;
		case 0x23:
			handle_NtQueryVirtualMemory(emu, context);
			break;
		case 0x31:
			handle_NtQueryPerformanceCounter(emu);
			break;
		case 0x36:
			handle_NtQuerySystemInformation(emu);
			break;
		case 0x48:
			handle_NtCreateEvent(emu, context);
			break;
		case 0x50:
			handle_NtProtectVirtualMemory(emu);
			break;
		case 0x5E:
			forward(c, handle_NtTraceEvent);
			break;
		case 0x78:
			handle_NtAllocateVirtualMemoryEx(emu);
			break;
		case 0xB2:
			forward(c, handle_NtCreateIoCompletion);
			break;
		case 0x11A:
			forward(c, handle_NtManageHotPatch);
			break;
		case 0x16E:
			handle_NtQuerySystemInformationEx(emu);
			break;
		default:
			printf("Unhandled syscall: %X\n", syscall_id);
			emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			emu.stop();
			break;
		}
	}
	catch (...)
	{
		emu.reg<uint64_t>(x64_register::rax, STATUS_UNSUCCESSFUL);
	}
}
