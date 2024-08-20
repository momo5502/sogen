#include "std_include.hpp"
#include "syscalls.hpp"

namespace
{
	void handle_NtQueryPerformanceCounter(const unicorn& uc)
	{
		const unicorn_object<LARGE_INTEGER> performance_counter{uc, uc.reg(UC_X86_REG_R10)};
		const unicorn_object<LARGE_INTEGER> performance_frequency{uc, uc.reg(UC_X86_REG_RDX)};

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

			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
		}
		catch (...)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_ACCESS_VIOLATION);
		}
	}

	void handle_NtManageHotPatch(const unicorn& uc)
	{
		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
	}

	void handle_NtOpenKey(const unicorn& uc)
	{
		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
	}

	void handle_NtCreateIoCompletion(const unicorn& uc)
	{
		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
	}

	void handle_NtTraceEvent(const unicorn& uc)
	{
		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
	}

	void handle_NtCreateEvent(const unicorn& uc, process_context& context)
	{
		const unicorn_object<uint64_t> event_handle{uc, uc.reg(UC_X86_REG_R10)};
		const auto object_attributes = uc.reg(UC_X86_REG_R8);
		const auto event_type = uc.reg<EVENT_TYPE>(UC_X86_REG_R9D);
		const auto initial_state = static_cast<BOOLEAN>(uc.read_stack(5));

		if (object_attributes)
		{
			puts("Unsupported object attributes");
			uc.stop();
			return;
		}

		const uint64_t index = context.events.size();
		event_handle.write(index);

		context.events.emplace_back(initial_state != FALSE, event_type);

		static_assert(sizeof(EVENT_TYPE) == sizeof(uint32_t));

		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
	}

	void handle_NtQueryVirtualMemory(const unicorn& uc, const process_context& context)
	{
		const auto process_handle = uc.reg(UC_X86_REG_R10);
		const auto base_address = uc.reg(UC_X86_REG_RDX);
		const auto info_class = uc.reg<uint32_t>(UC_X86_REG_R8D);
		const auto memory_information = uc.reg(UC_X86_REG_R9);
		const auto memory_information_length = static_cast<uint32_t>(uc.read_stack(5));
		const unicorn_object<uint32_t> return_length{uc, uc.read_stack(6)};

		if (process_handle != ~0ULL)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			return;
		}

		if (info_class == MemoryWorkingSetExInformation)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			return;
		}

		if (info_class != MemoryImageInformation)
		{
			printf("Unsupported memory info class: %X\n", info_class);
			uc.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(MEMORY_IMAGE_INFORMATION));
		}

		if (memory_information_length != sizeof(MEMORY_IMAGE_INFORMATION))
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_BUFFER_OVERFLOW);
			return;
		}

		if (!is_within_start_and_length(base_address, context.ntdll.image_base, context.ntdll.size_of_image))
		{
			puts("Bad image request");
			uc.stop();
			return;
		}

		const unicorn_object<MEMORY_IMAGE_INFORMATION> info{uc, memory_information};

		info.access([&](MEMORY_IMAGE_INFORMATION& image_info)
		{
			image_info.ImageBase = reinterpret_cast<void*>(context.ntdll.image_base);
			image_info.SizeOfImage = context.ntdll.size_of_image;
		});

		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
	}

	void handle_NtQuerySystemInformation(const unicorn& uc)
	{
		const auto info_class = uc.reg<uint32_t>(UC_X86_REG_R10D);
		const auto system_information = uc.reg(UC_X86_REG_RDX);
		const auto system_information_length = uc.reg<uint32_t>(UC_X86_REG_R8D);
		const unicorn_object<uint32_t> return_length{uc, uc.reg(UC_X86_REG_R9)};

		if (info_class == SystemFlushInformation
			|| info_class == SystemHypervisorSharedPageInformation)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
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
				uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_BUFFER_TOO_SMALL);
				return;
			}

			const unicorn_object<SYSTEM_NUMA_INFORMATION> info_obj{uc, system_information};

			info_obj.access([&](SYSTEM_NUMA_INFORMATION& info)
			{
				memset(&info, 0, sizeof(info));
				info.ActiveProcessorsGroupAffinity->Mask = 0xFFF;
				info.AvailableMemory[0] = 0xFFF;
				info.Pad[0] = 0xFFF;
			});

			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
			return;
		}

		if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
		{
			printf("Unsupported system info class: %X\n", info_class);
			uc.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(SYSTEM_BASIC_INFORMATION));
		}

		if (system_information_length != sizeof(SYSTEM_BASIC_INFORMATION))
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_BUFFER_TOO_SMALL);
			return;
		}

		const unicorn_object<SYSTEM_BASIC_INFORMATION> info{uc, system_information};

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

		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
	}

	void handle_NtQuerySystemInformationEx(const unicorn& uc)
	{
		const auto info_class = uc.reg<uint32_t>(UC_X86_REG_R10D);
		const auto input_buffer = uc.reg(UC_X86_REG_RDX);
		const auto input_buffer_length = uc.reg<uint32_t>(UC_X86_REG_R8D);
		const auto system_information = uc.reg(UC_X86_REG_R9);
		const auto system_information_length = static_cast<uint32_t>(uc.read_stack(5));
		const unicorn_object<uint32_t> return_length{uc, uc.read_stack(6)};

		if (info_class == SystemFlushInformation
			|| info_class == SystemFeatureConfigurationInformation
			|| info_class == SystemFeatureConfigurationSectionInformation)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
			return;
		}

		if (info_class == SystemLogicalProcessorAndGroupInformation)
		{
			void* buffer = calloc(1, input_buffer_length);
			void* res_buff = calloc(1, system_information_length);
			uc_mem_read(uc, input_buffer, buffer, input_buffer_length);

			uint64_t code = 0;

			return_length.access([&](uint32_t& len)
			{
				code = NtQuerySystemInformationEx((SYSTEM_INFORMATION_CLASS)info_class, buffer, input_buffer_length,
				                                  res_buff,
				                                  system_information_length, (ULONG*)&len);
			});

			if (code == 0)
			{
				uc_mem_write(uc, system_information, res_buff, return_length.read());
			}

			free(buffer);
			free(res_buff);

			uc.reg<uint64_t>(UC_X86_REG_RAX, code);
			return;
		}

		if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
		{
			printf("Unsupported system info ex class: %X\n", info_class);
			uc.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(SYSTEM_BASIC_INFORMATION));
		}

		if (system_information_length != sizeof(SYSTEM_BASIC_INFORMATION))
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_BUFFER_TOO_SMALL);
			return;
		}

		const unicorn_object<SYSTEM_BASIC_INFORMATION> info{uc, system_information};

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

		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
	}

	void handle_NtQueryProcessInformation(const unicorn& uc)
	{
		const auto process_handle = uc.reg<uint64_t>(UC_X86_REG_R10);
		const auto info_class = uc.reg<uint32_t>(UC_X86_REG_EDX);
		const auto process_information = uc.reg(UC_X86_REG_R8);
		const auto process_information_length = uc.reg<uint32_t>(UC_X86_REG_R9D);
		const unicorn_object<uint32_t> return_length{uc, uc.read_stack(5)};

		if (process_handle != ~0ULL)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			return;
		}

		if (info_class != ProcessCookie)
		{
			printf("Unsupported process info class: %X\n", info_class);
			uc.stop();
			return;
		}

		if (return_length)
		{
			return_length.write(sizeof(uint32_t));
		}

		if (process_information_length != sizeof(uint32_t))
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_BUFFER_OVERFLOW);
			return;
		}

		const unicorn_object<uint32_t> info{uc, process_information};
		info.write(0x01234567);

		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
	}

	void handle_NtProtectVirtualMemory(const unicorn& uc)
	{
		const auto process_handle = uc.reg(UC_X86_REG_R10);
		const unicorn_object<uint64_t> base_address{uc, uc.reg(UC_X86_REG_RDX)};
		const unicorn_object<uint32_t> bytes_to_protect{uc, uc.reg(UC_X86_REG_R8)};
		const auto protection = uc.reg<uint32_t>(UC_X86_REG_R9D);
		const unicorn_object<uint32_t> old_protection{uc, uc.read_stack(5)};

		if (process_handle != ~0ULL)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			return;
		}

		const auto address = page_align_down(base_address.read());
		base_address.write(address);

		const auto size = page_align_up(bytes_to_protect.read());
		bytes_to_protect.write(static_cast<uint32_t>(size));

		const auto current_uc_protection = get_memory_protection(uc, address);
		const auto current_protection = map_unicorn_to_nt_protection(current_uc_protection);
		old_protection.write(current_protection);

		const auto requested_protection = map_nt_to_unicorn_protection(protection);
		uce(uc_mem_protect(uc, address, size, requested_protection));

		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
	}

	void handle_NtAllocateVirtualMemory(const unicorn& uc)
	{
		const auto process_handle = uc.reg(UC_X86_REG_R10);
		const unicorn_object<uint64_t> base_address{uc, uc.reg(UC_X86_REG_RDX)};
		const unicorn_object<uint64_t> bytes_to_allocate{uc, uc.reg(UC_X86_REG_R9)};
		//const auto allocation_type = uc.reg<uint32_t>(UC_X86_REG_R9D);
		const auto page_protection = static_cast<uint32_t>(uc.read_stack(6));

		if (process_handle != ~0ULL)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			return;
		}

		constexpr auto allocation_granularity = 0x10000;
		const auto allocation_bytes = bytes_to_allocate.read();
		//allocation_bytes = align_up(allocation_bytes, allocation_granularity);
		//bytes_to_allocate.write(allocation_bytes);

		const auto protection = map_nt_to_unicorn_protection(page_protection);

		auto allocate_anywhere = false;
		auto allocation_base = base_address.read();
		if (!allocation_base)
		{
			allocate_anywhere = true;
			allocation_base = allocation_granularity;
		}
		else if (is_memory_allocated(uc, allocation_base))
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
			return;
		}

		bool succeeded = false;

		while (true)
		{
			succeeded = uc_mem_map(uc, allocation_base, allocation_bytes, protection) == UC_ERR_OK;
			if (succeeded || !allocate_anywhere)
			{
				break;
			}

			allocation_base += allocation_granularity;
		}

		base_address.write(allocation_base);

		uc.reg<uint64_t>(UC_X86_REG_RAX, succeeded
			                                 ? STATUS_SUCCESS
			                                 : STATUS_NOT_SUPPORTED // No idea what the correct code is
		);
	}

	void handle_NtAllocateVirtualMemoryEx(const unicorn& uc)
	{
		const auto process_handle = uc.reg(UC_X86_REG_R10);
		const unicorn_object<uint64_t> base_address{uc, uc.reg(UC_X86_REG_RDX)};
		const unicorn_object<uint64_t> bytes_to_allocate{uc, uc.reg(UC_X86_REG_R8)};
		//const auto allocation_type = uc.reg<uint32_t>(UC_X86_REG_R9D);
		const auto page_protection = static_cast<uint32_t>(uc.read_stack(5));

		if (process_handle != ~0ULL)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			return;
		}

		constexpr auto allocation_granularity = 0x10000;
		const auto allocation_bytes = bytes_to_allocate.read();
		//allocation_bytes = align_up(allocation_bytes, allocation_granularity);
		//bytes_to_allocate.write(allocation_bytes);

		const auto protection = map_nt_to_unicorn_protection(page_protection);

		auto allocate_anywhere = false;
		auto allocation_base = base_address.read();
		if (!allocation_base)
		{
			allocate_anywhere = true;
			allocation_base = allocation_granularity;
		}
		else if (is_memory_allocated(uc, allocation_base))
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
			return;
		}

		bool succeeded = false;

		while (true)
		{
			succeeded = uc_mem_map(uc, allocation_base, allocation_bytes, protection) == UC_ERR_OK;
			if (succeeded || !allocate_anywhere)
			{
				break;
			}

			allocation_base += allocation_granularity;
		}

		base_address.write(allocation_base);

		uc.reg<uint64_t>(UC_X86_REG_RAX, succeeded
			                                 ? STATUS_SUCCESS
			                                 : STATUS_NOT_SUPPORTED // No idea what the correct code is
		);
	}

	void handle_NtFreeVirtualMemory(const unicorn& uc)
	{
		const auto process_handle = uc.reg(UC_X86_REG_R10);
		const unicorn_object<uint64_t> base_address{uc, uc.reg(UC_X86_REG_RDX)};
		const unicorn_object<uint64_t> bytes_to_allocate{uc, uc.reg(UC_X86_REG_R8)};

		if (process_handle != ~0ULL)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			return;
		}

		const auto allocation_base = base_address.read();
		const auto allocation_size = bytes_to_allocate.read();

		const auto succeeded = uc_mem_unmap(uc, allocation_base, allocation_size) == UC_ERR_OK;

		uc.reg<uint64_t>(UC_X86_REG_RAX, succeeded
			                                 ? STATUS_SUCCESS
			                                 : STATUS_NOT_SUPPORTED // No idea what the correct code is
		);
	}
}

void handle_syscall(const unicorn& uc, process_context& context)
{
	const auto address = uc.reg(UC_X86_REG_RIP);
	const auto syscall_id = uc.reg<uint32_t>(UC_X86_REG_EAX);

	printf("Handling syscall: %X (%llX)\n", syscall_id, address);

	try
	{
		switch (syscall_id)
		{
		case 0x12:
			handle_NtOpenKey(uc);
			break;
		case 0x18:
			handle_NtAllocateVirtualMemory(uc);
			break;
		case 0x1E:
			handle_NtFreeVirtualMemory(uc);
			break;
		case 0x19:
			handle_NtQueryProcessInformation(uc);
			break;
		case 0x23:
			handle_NtQueryVirtualMemory(uc, context);
			break;
		case 0x31:
			handle_NtQueryPerformanceCounter(uc);
			break;
		case 0x36:
			handle_NtQuerySystemInformation(uc);
			break;
		case 0x48:
			handle_NtCreateEvent(uc, context);
			break;
		case 0x50:
			handle_NtProtectVirtualMemory(uc);
			break;
		case 0x5E:
			handle_NtTraceEvent(uc);
			break;
		case 0x78:
			handle_NtAllocateVirtualMemoryEx(uc);
			break;
		case 0xB2:
			handle_NtCreateIoCompletion(uc);
			break;
		case 0x11A:
			handle_NtManageHotPatch(uc);
			break;
		case 0x16E:
			handle_NtQuerySystemInformationEx(uc);
			break;
		default:
			printf("Unhandled syscall: %X\n", syscall_id);
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_IMPLEMENTED);
			uc.stop();
			break;
		}
	}
	catch (...)
	{
		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_UNSUCCESSFUL);
	}
}
