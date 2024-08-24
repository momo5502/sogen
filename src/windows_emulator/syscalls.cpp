#include "std_include.hpp"
#include "syscalls.hpp"

namespace
{
	struct syscall_context
	{
		x64_emulator& emu;
		process_context& proc;
	};

	constexpr uint64_t EVENT_BIT = 1ULL << 63ULL;

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
		requires(std::is_integral_v<T> || std::is_enum_v<T>)
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
		c.emu.reg<int64_t>(x64_register::rax, ret);
	}

	NTSTATUS handle_NtQueryPerformanceCounter(const syscall_context&,
	                                          const emulator_object<LARGE_INTEGER> performance_counter,
	                                          const emulator_object<LARGE_INTEGER> performance_frequency)
	{
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

			return STATUS_SUCCESS;
		}
		catch (...)
		{
			return STATUS_ACCESS_VIOLATION;
		}
	}

	NTSTATUS handle_NtManageHotPatch()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtCreateWorkerFactory()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtOpenKey()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtSetEvent(const syscall_context& c, const uint64_t handle,
	                           const emulator_object<LONG> previous_state)
	{
		if (handle & EVENT_BIT)
		{
			const auto event_index = static_cast<uint32_t>(handle & ~EVENT_BIT);
			const auto entry = c.proc.events.find(event_index);
			if (entry != c.proc.events.end())
			{
				if (previous_state.value())
				{
					previous_state.write(entry->second.signaled ? 1ULL : 0ULL);
				}

				entry->second.signaled = true;
				return STATUS_SUCCESS;
			}
		}

		return STATUS_INVALID_HANDLE;
	}

	NTSTATUS handle_NtClose(const syscall_context& c, const uint64_t handle)
	{
		if (handle & EVENT_BIT)
		{
			const auto event_index = static_cast<uint32_t>(handle & ~EVENT_BIT);
			const auto entry = c.proc.events.find(event_index);
			if (entry != c.proc.events.end())
			{
				c.proc.events.erase(entry);
				return STATUS_SUCCESS;
			}
		}

		return STATUS_INVALID_HANDLE;
	}

	NTSTATUS handle_NtTraceEvent()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtCreateEvent(const syscall_context& c, const emulator_object<uint64_t> event_handle,
	                              const ACCESS_MASK /*desired_access*/, const uint64_t object_attributes,
	                              const EVENT_TYPE event_type, const BOOLEAN initial_state)
	{
		if (object_attributes)
		{
			puts("Unsupported object attributes");
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		uint32_t index = 1;
		for (;; ++index)
		{
			if (!c.proc.events.contains(index))
			{
				break;
			}
		}

		event_handle.write(index | EVENT_BIT);

		c.proc.events.try_emplace(index, initial_state != FALSE, event_type);

		static_assert(sizeof(EVENT_TYPE) == sizeof(uint32_t));
		static_assert(sizeof(ACCESS_MASK) == sizeof(uint32_t));

		return STATUS_SUCCESS;
	}


	NTSTATUS handle_NtCreateIoCompletion(const syscall_context& c, const emulator_object<uint64_t> event_handle,
	                                     const ACCESS_MASK desired_access, const uint64_t object_attributes,
	                                     uint32_t /*number_of_concurrent_threads*/)
	{
		return handle_NtCreateEvent(c, event_handle, desired_access, object_attributes, NotificationEvent, FALSE);
	}

	NTSTATUS handle_NtCreateWaitCompletionPacket(const syscall_context& c, const emulator_object<uint64_t> event_handle,
	                                             const ACCESS_MASK desired_access, const uint64_t object_attributes)
	{
		return handle_NtCreateEvent(c, event_handle, desired_access, object_attributes, NotificationEvent, FALSE);
	}

	NTSTATUS handle_NtQueryVirtualMemory(const syscall_context& c, const uint64_t process_handle,
	                                     const uint64_t base_address, const uint32_t info_class,
	                                     const uint64_t memory_information, const uint32_t memory_information_length,
	                                     const emulator_object<uint32_t> return_length)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_IMPLEMENTED;
		}

		if (info_class == MemoryWorkingSetExInformation)
		{
			return STATUS_NOT_IMPLEMENTED;
		}

		if (info_class != MemoryImageInformation)
		{
			printf("Unsupported memory info class: %X\n", info_class);
			c.emu.stop();
			return STATUS_NOT_IMPLEMENTED;
		}

		if (return_length)
		{
			return_length.write(sizeof(MEMORY_IMAGE_INFORMATION));
		}

		if (memory_information_length != sizeof(MEMORY_IMAGE_INFORMATION))
		{
			return STATUS_BUFFER_OVERFLOW;
		}

		if (!is_within_start_and_length(base_address, c.proc.ntdll.image_base, c.proc.ntdll.size_of_image))
		{
			puts("Bad image request");
			c.emu.stop();
			return STATUS_NOT_IMPLEMENTED;
		}

		const emulator_object<MEMORY_IMAGE_INFORMATION> info{c.emu, memory_information};

		info.access([&](MEMORY_IMAGE_INFORMATION& image_info)
		{
			image_info.ImageBase = reinterpret_cast<void*>(c.proc.ntdll.image_base);
			image_info.SizeOfImage = c.proc.ntdll.size_of_image;
		});

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, const uint32_t info_class,
	                                         const uint64_t system_information,
	                                         const uint32_t system_information_length,
	                                         const emulator_object<uint32_t> return_length)
	{
		if (info_class == SystemFlushInformation
			|| info_class == SystemHypervisorSharedPageInformation)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == SystemNumaProcessorMap)
		{
			if (return_length)
			{
				return_length.write(sizeof(SYSTEM_NUMA_INFORMATION));
			}

			if (system_information_length != sizeof(SYSTEM_NUMA_INFORMATION))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const emulator_object<SYSTEM_NUMA_INFORMATION> info_obj{c.emu, system_information};

			info_obj.access([&](SYSTEM_NUMA_INFORMATION& info)
			{
				memset(&info, 0, sizeof(info));
				info.ActiveProcessorsGroupAffinity->Mask = 0xFFF;
				info.AvailableMemory[0] = 0xFFF;
				info.Pad[0] = 0xFFF;
			});

			return STATUS_SUCCESS;
		}

		if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
		{
			printf("Unsupported system info class: %X\n", info_class);
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		if (return_length)
		{
			return_length.write(sizeof(SYSTEM_BASIC_INFORMATION));
		}

		if (system_information_length != sizeof(SYSTEM_BASIC_INFORMATION))
		{
			return STATUS_BUFFER_TOO_SMALL;
		}

		const emulator_object<SYSTEM_BASIC_INFORMATION> info{c.emu, system_information};

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

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, const uint32_t info_class,
	                                           const uint64_t input_buffer,
	                                           const uint32_t input_buffer_length,
	                                           const uint64_t system_information,
	                                           const uint32_t system_information_length,
	                                           const emulator_object<uint32_t> return_length)
	{
		if (info_class == SystemFlushInformation
			|| info_class == SystemFeatureConfigurationInformation
			|| info_class == SystemFeatureConfigurationSectionInformation)
		{
			printf("Unsupported, but allowed system info class: %X\n", info_class);
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == SystemLogicalProcessorAndGroupInformation)
		{
			void* buffer = calloc(1, input_buffer_length);
			void* res_buff = calloc(1, system_information_length);
			c.emu.read_memory(input_buffer, buffer, input_buffer_length);

			NTSTATUS code = STATUS_SUCCESS;

			return_length.access([&](uint32_t& len)
			{
				code = NtQuerySystemInformationEx(static_cast<SYSTEM_INFORMATION_CLASS>(info_class), buffer,
				                                  input_buffer_length,
				                                  res_buff,
				                                  system_information_length, reinterpret_cast<ULONG*>(&len));
			});

			if (code == 0)
			{
				c.emu.write_memory(system_information, res_buff, return_length.read());
			}

			free(buffer);
			free(res_buff);

			return code;
		}

		if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
		{
			printf("Unsupported system info ex class: %X\n", info_class);
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		if (return_length)
		{
			return_length.write(sizeof(SYSTEM_BASIC_INFORMATION));
		}

		if (system_information_length != sizeof(SYSTEM_BASIC_INFORMATION))
		{
			return STATUS_BUFFER_TOO_SMALL;
		}

		const emulator_object<SYSTEM_BASIC_INFORMATION> info{c.emu, system_information};

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

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtQueryProcessInformation(const syscall_context& c, const uint64_t process_handle,
	                                          const uint32_t info_class, const uint64_t process_information,
	                                          const uint32_t process_information_length,
	                                          const emulator_object<uint32_t> return_length)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_IMPLEMENTED;
		}

		if (info_class != ProcessCookie)
		{
			printf("Unsupported process info class: %X\n", info_class);
			c.emu.stop();

			return STATUS_NOT_IMPLEMENTED;
		}

		if (return_length)
		{
			return_length.write(sizeof(uint32_t));
		}

		if (process_information_length != sizeof(uint32_t))
		{
			return STATUS_BUFFER_OVERFLOW;
		}

		const emulator_object<uint32_t> info{c.emu, process_information};
		info.write(0x01234567);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, const uint64_t process_handle,
	                                       const emulator_object<uint64_t> base_address,
	                                       const emulator_object<uint32_t> bytes_to_protect,
	                                       const uint32_t protection,
	                                       const emulator_object<uint32_t> old_protection)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_IMPLEMENTED;
		}

		const auto address = page_align_down(base_address.read());
		base_address.write(address);

		const auto size = page_align_up(bytes_to_protect.read());
		bytes_to_protect.write(static_cast<uint32_t>(size));

		const auto requested_protection = map_nt_to_emulator_protection(protection);

		memory_permission old_protection_value{};
		c.emu.protect_memory(address, size, requested_protection, &old_protection_value);

		const auto current_protection = map_emulator_to_nt_protection(old_protection_value);
		old_protection.write(current_protection);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, const uint64_t process_handle,
	                                          const emulator_object<uint64_t> base_address,
	                                          const emulator_object<uint64_t> bytes_to_allocate,
	                                          const uint32_t allocation_type,
	                                          const uint32_t page_protection)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_IMPLEMENTED;
		}

		const auto allocation_bytes = bytes_to_allocate.read();

		const auto protection = map_nt_to_emulator_protection(page_protection);

		auto potential_base = base_address.read();
		if (!potential_base)
		{
			potential_base = c.emu.find_free_allocation_base(allocation_bytes);
		}

		if (!potential_base)
		{
			return STATUS_MEMORY_NOT_ALLOCATED;
		}

		base_address.write(potential_base);

		const bool reserve = allocation_type & MEM_RESERVE;
		const bool commit = allocation_type & MEM_COMMIT;

		if ((allocation_type & ~(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN)) || (!commit && !reserve))
		{
			throw std::runtime_error("Unsupported allocation type!");
		}

		if (commit && !reserve)
		{
			return c.emu.commit_memory(potential_base, allocation_bytes, protection)
				       ? STATUS_SUCCESS
				       : STATUS_MEMORY_NOT_ALLOCATED;
		}

		return c.emu.allocate_memory(potential_base, allocation_bytes, protection, !commit)
			       ? STATUS_SUCCESS
			       : STATUS_MEMORY_NOT_ALLOCATED;
	}

	NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, const uint64_t process_handle,
	                                        const emulator_object<uint64_t> base_address,
	                                        uint64_t /*zero_bits*/,
	                                        const emulator_object<uint64_t> bytes_to_allocate,
	                                        const uint32_t allocation_type, const uint32_t page_protection)
	{
		return handle_NtAllocateVirtualMemoryEx(c, process_handle, base_address, bytes_to_allocate, allocation_type,
		                                        page_protection);
	}

	NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, const uint64_t process_handle,
	                                    const emulator_object<uint64_t> base_address,
	                                    const emulator_object<uint64_t> bytes_to_allocate, uint32_t free_type)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_IMPLEMENTED;
		}

		const auto allocation_base = base_address.read();
		const auto allocation_size = bytes_to_allocate.read();

		if (free_type & MEM_RELEASE)
		{
			return c.emu.release_memory(allocation_base, allocation_size)
				       ? STATUS_SUCCESS
				       : STATUS_MEMORY_NOT_ALLOCATED;
		}

		if (free_type & MEM_DECOMMIT)
		{
			return c.emu.decommit_memory(allocation_base, allocation_size)
				       ? STATUS_SUCCESS
				       : STATUS_MEMORY_NOT_ALLOCATED;
		}

		throw std::runtime_error("Bad free type");
	}

#define handle(id, handler) \
	case id: \
		forward(c, handler);\
		break

	void dispatch_syscall(const syscall_context& c, const uint32_t syscall_id)
	{
		switch (syscall_id)
		{
		handle(0x00E, handle_NtSetEvent);
		handle(0x00F, handle_NtClose);
		handle(0x012, handle_NtOpenKey);
		handle(0x018, handle_NtAllocateVirtualMemory);
		handle(0x019, handle_NtQueryProcessInformation);
		handle(0x01E, handle_NtFreeVirtualMemory);
		handle(0x023, handle_NtQueryVirtualMemory);
		handle(0x031, handle_NtQueryPerformanceCounter);
		handle(0x036, handle_NtQuerySystemInformation);
		handle(0x048, handle_NtCreateEvent);
		handle(0x050, handle_NtProtectVirtualMemory);
		handle(0x05E, handle_NtTraceEvent);
		handle(0x078, handle_NtAllocateVirtualMemoryEx);
		handle(0x0B2, handle_NtCreateIoCompletion);
		handle(0x0D2, handle_NtCreateWaitCompletionPacket);
		handle(0x0D5, handle_NtCreateWorkerFactory);
		handle(0x11A, handle_NtManageHotPatch);
		handle(0x16E, handle_NtQuerySystemInformationEx);

		default:
			printf("Unhandled syscall: %X\n", syscall_id);
			c.emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_IMPLEMENTED);
			c.emu.stop();
			break;
		}
	}


#undef handle
}


void handle_syscall(x64_emulator& emu, process_context& context)
{
	const auto address = emu.read_instruction_pointer();
	const auto syscall_id = emu.reg<uint32_t>(x64_register::eax);

	printf("Handling syscall: %X (%llX)\n", syscall_id, address);

	const syscall_context c{emu, context};

	try
	{
		dispatch_syscall(c, syscall_id);
	}
	catch (std::exception& e)
	{
		printf("Syscall threw an exception: %X (%llX) - %s\n", syscall_id, address, e.what());
		emu.reg<uint64_t>(x64_register::rax, STATUS_UNSUCCESSFUL);
		emu.stop();
	}
	catch (...)
	{
		printf("Syscall threw an unknown exception: %X (%llX)\n", syscall_id, address);
		emu.reg<uint64_t>(x64_register::rax, STATUS_UNSUCCESSFUL);
		emu.stop();
	}
}
