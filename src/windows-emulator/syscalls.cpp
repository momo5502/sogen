#include "std_include.hpp"
#include "syscall_dispatcher.hpp"

#include <numeric>

#include "context_frame.hpp"
#include "emulator_utils.hpp"
#include "syscall_utils.hpp"

#include <utils/io.hpp>

namespace
{
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

	NTSTATUS handle_NtOpenKey(const syscall_context& c, const emulator_object<uint64_t> /*key_handle*/,
	                          const ACCESS_MASK /*desired_access*/,
	                          const emulator_object<OBJECT_ATTRIBUTES> object_attributes)
	{
		const auto attributes = object_attributes.read();
		const auto key = read_unicode_string(c.emu, attributes.ObjectName);

		c.win_emu.logger.print(color::dark_gray, "--> Registry key: %S\n", key.c_str());

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtOpenKeyEx()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtSetInformationThread(const syscall_context& c, const uint64_t thread_handle,
	                                       const THREADINFOCLASS info_class,
	                                       const uint64_t thread_information,
	                                       const uint32_t thread_information_length)
	{
		auto* thread = thread_handle == ~1ULL
			               ? c.proc.active_thread
			               : c.proc.threads.get(thread_handle);

		if (!thread)
		{
			return STATUS_INVALID_HANDLE;
		}

		if (info_class == ThreadSchedulerSharedDataSlot)
		{
			return STATUS_SUCCESS;
		}

		if (info_class == ThreadNameInformation)
		{
			if (thread_information_length != sizeof(THREAD_NAME_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<THREAD_NAME_INFORMATION> info{c.emu, thread_information};
			const auto i = info.read();
			thread->name = read_unicode_string(c.emu, i.ThreadName);

			c.win_emu.logger.print(color::blue, "Setting thread (%d) name: %S\n", thread->id, thread->name.c_str());

			return STATUS_SUCCESS;
		}

		printf("Unsupported thread info class: %X\n", info_class);
		c.emu.stop();
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtSetEvent(const syscall_context& c, const uint64_t handle,
	                           const emulator_object<LONG> previous_state)
	{
		const auto entry = c.proc.events.get(handle);
		if (!entry)
		{
			return STATUS_INVALID_HANDLE;
		}

		if (previous_state.value())
		{
			previous_state.write(entry->signaled ? 1ULL : 0ULL);
		}

		entry->signaled = true;
		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtClose(const syscall_context& c, const uint64_t handle)
	{
		const auto value = get_handle_value(handle);
		if (value.is_pseudo)
		{
			return STATUS_SUCCESS;
		}

		if (value.type == handle_types::thread && c.proc.threads.erase(handle))
		{
			return STATUS_SUCCESS;
		}

		if (value.type == handle_types::event && c.proc.events.erase(handle))
		{
			return STATUS_SUCCESS;
		}

		if (value.type == handle_types::file && c.proc.files.erase(handle))
		{
			return STATUS_SUCCESS;
		}

		if (value.type == handle_types::semaphore && c.proc.semaphores.erase(handle))
		{
			return STATUS_SUCCESS;
		}

		return STATUS_INVALID_HANDLE;
	}

	NTSTATUS handle_NtTraceEvent()
	{
		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtOpenThreadToken()
	{
		return STATUS_NO_TOKEN;
	}

	NTSTATUS handle_NtCreateEvent(const syscall_context& c, const emulator_object<uint64_t> event_handle,
	                              const ACCESS_MASK /*desired_access*/,
	                              const emulator_object<OBJECT_ATTRIBUTES> object_attributes,
	                              const EVENT_TYPE event_type, const BOOLEAN initial_state)
	{
		std::wstring name{};
		if (object_attributes)
		{
			const auto attributes = object_attributes.read();
			if (attributes.ObjectName)
			{
				name = read_unicode_string(c.emu, attributes.ObjectName);
			}
		}

		event e{};
		e.type = event_type;
		e.signaled = initial_state != FALSE;
		e.name = std::move(name);

		const auto handle = c.proc.events.store(std::move(e));
		event_handle.write(handle.bits);

		static_assert(sizeof(EVENT_TYPE) == sizeof(uint32_t));
		static_assert(sizeof(ACCESS_MASK) == sizeof(uint32_t));

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtOpenEvent(const syscall_context& c, const emulator_object<uint64_t> event_handle,
	                            const ACCESS_MASK /*desired_access*/,
	                            const emulator_object<OBJECT_ATTRIBUTES> object_attributes)
	{
		const auto attributes = object_attributes.read();
		const auto name = read_unicode_string(c.emu, attributes.ObjectName);

		for (auto& entry : c.proc.events)
		{
			if (entry.second.name == name)
			{
				++entry.second.ref_count;
				event_handle.write(c.proc.events.make_handle(entry.first).bits);
				return STATUS_SUCCESS;
			}
		}

		return STATUS_NOT_FOUND;
	}

	NTSTATUS handle_NtQueryVolumeInformationFile(const syscall_context& c, uint64_t file_handle,
	                                             uint64_t /*io_status_block*/, uint64_t fs_information,
	                                             ULONG /*length*/,
	                                             FS_INFORMATION_CLASS fs_information_class)
	{
		if (fs_information_class != FileFsDeviceInformation)
		{
			printf("Unsupported fs info class: %X\n", fs_information_class);
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		const emulator_object<FILE_FS_DEVICE_INFORMATION> info_obj{c.emu, fs_information};
		info_obj.access([&](FILE_FS_DEVICE_INFORMATION& info)
		{
			if (file_handle == STDOUT_HANDLE.bits && !c.win_emu.buffer_stdout)
			{
				info.DeviceType = FILE_DEVICE_CONSOLE;
				info.Characteristics = 0x20000;
			}
			else
			{
				info.DeviceType = FILE_DEVICE_DISK;
				info.Characteristics = 0x20020;
			}
		});

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtOpenSection(const syscall_context& c, const emulator_object<uint64_t> section_handle,
	                              const ACCESS_MASK /*desired_access*/,
	                              const emulator_object<OBJECT_ATTRIBUTES> object_attributes)
	{
		const auto attributes = object_attributes.read();

		auto filename = read_unicode_string(c.emu, attributes.ObjectName);
		c.win_emu.logger.print(color::gray, "Opening section: %S\n", filename.c_str());

		if (filename == L"\\Windows\\SharedSection")
		{
			section_handle.write(SHARED_SECTION.bits);
			return STATUS_SUCCESS;
		}

		if (reinterpret_cast<uint64_t>(attributes.RootDirectory) != KNOWN_DLLS_DIRECTORY)
		{
			puts("Unsupported section");
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		filename = L"C:\\WINDOWS\\System32\\" + filename;
		if (!std::filesystem::exists(filename))
		{
			return STATUS_FILE_INVALID;
		}

		file f{};
		f.name = std::move(filename);

		const auto handle = c.proc.files.store(std::move(f));
		section_handle.write(handle.bits);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtMapViewOfSection(const syscall_context& c, uint64_t section_handle, uint64_t process_handle,
	                                   emulator_object<uint64_t> base_address, ULONG_PTR /*zero_bits*/,
	                                   SIZE_T /*commit_size*/,
	                                   const emulator_object<LARGE_INTEGER> /*section_offset*/,
	                                   const emulator_object<SIZE_T> view_size, SECTION_INHERIT /*inherit_disposition*/,
	                                   ULONG /*allocation_type*/, ULONG /*win32_protect*/)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_INVALID_HANDLE;
		}

		if (section_handle == SHARED_SECTION)
		{
			const auto address = c.emu.find_free_allocation_base(c.proc.shared_section_size);
			c.emu.allocate_memory(address,
			                      c.proc.shared_section_size, memory_permission::read_write);

			size_t windows_dir_size{};
			c.proc.kusd.access([&](const KUSER_SHARED_DATA& kusd)
			{
				const std::wstring_view windows_dir = kusd.NtSystemRoot.arr;
				windows_dir_size = windows_dir.size() * 2;
			});

			constexpr auto windows_dir_offset = 0x10;
			c.emu.write_memory(address + 8, windows_dir_offset);

			const auto obj_address = address + windows_dir_offset;

			const emulator_object<UNICODE_STRING> windir_obj{c.emu, obj_address};
			windir_obj.access([&](UNICODE_STRING& ucs)
			{
				const auto dir_address = c.proc.kusd.value() + offsetof(KUSER_SHARED_DATA, NtSystemRoot);

				ucs.Buffer = reinterpret_cast<wchar_t*>(dir_address - obj_address);
				ucs.Length = static_cast<uint16_t>(windows_dir_size);
				ucs.MaximumLength = ucs.Length;
			});


			const emulator_object<UNICODE_STRING> sysdir_obj{c.emu, obj_address + windir_obj.size()};
			sysdir_obj.access([&](UNICODE_STRING& ucs)
			{
				c.proc.base_allocator.make_unicode_string(ucs, L"C:\\WINDOWS\\System32");
				ucs.Buffer = reinterpret_cast<wchar_t*>(reinterpret_cast<uint64_t>(ucs.Buffer) - obj_address);
			});

			if (view_size.value())
			{
				view_size.write(c.proc.shared_section_size);
			}

			base_address.write(address);

			return STATUS_SUCCESS;
		}

		const auto section_entry = c.proc.files.get(section_handle);
		if (!section_entry)
		{
			return STATUS_INVALID_HANDLE;
		}

		const auto binary = c.proc.module_manager.map_module(section_entry->name, c.win_emu.logger);
		if (!binary)
		{
			return STATUS_FILE_INVALID;
		}

		if (view_size.value())
		{
			view_size.write(binary->size_of_image);
		}

		base_address.write(binary->image_base);

		return STATUS_SUCCESS;
	}


	NTSTATUS handle_NtCreateIoCompletion(const syscall_context& c, const emulator_object<uint64_t> event_handle,
	                                     const ACCESS_MASK desired_access,
	                                     const emulator_object<OBJECT_ATTRIBUTES> object_attributes,
	                                     uint32_t /*number_of_concurrent_threads*/)
	{
		return handle_NtCreateEvent(c, event_handle, desired_access, object_attributes, NotificationEvent, FALSE);
	}

	NTSTATUS handle_NtCreateWaitCompletionPacket(const syscall_context& c, const emulator_object<uint64_t> event_handle,
	                                             const ACCESS_MASK desired_access,
	                                             const emulator_object<OBJECT_ATTRIBUTES> object_attributes)
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
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == MemoryWorkingSetExInformation
			|| info_class == MemoryImageExtensionInformation)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == MemoryBasicInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(MEMORY_BASIC_INFORMATION));
			}

			if (memory_information_length != sizeof(MEMORY_BASIC_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<MEMORY_BASIC_INFORMATION> info{c.emu, memory_information};

			info.access([&](MEMORY_BASIC_INFORMATION& image_info)
			{
				const auto region_info = c.emu.get_region_info(base_address);

				assert(!region_info.is_committed || region_info.is_reserved);

				image_info.BaseAddress = reinterpret_cast<void*>(region_info.start);
				image_info.AllocationBase = reinterpret_cast<void*>(region_info.allocation_base);
				image_info.AllocationProtect = 0;
				image_info.PartitionId = 0;
				image_info.RegionSize = region_info.length;
				image_info.State = region_info.is_committed
					                   ? MEM_COMMIT
					                   : (region_info.is_reserved
						                      ? MEM_RESERVE
						                      : MEM_FREE);
				image_info.Protect = map_emulator_to_nt_protection(region_info.pemissions);
				image_info.Type = MEM_PRIVATE;
			});

			return STATUS_SUCCESS;
		}

		if (info_class == MemoryImageInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(MEMORY_IMAGE_INFORMATION));
			}

			if (memory_information_length != sizeof(MEMORY_IMAGE_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const auto mod = c.proc.module_manager.find_by_address(base_address);
			if (!mod)
			{
				printf("Bad address for memory image request: 0x%llX\n", base_address);
				return STATUS_INVALID_ADDRESS;
			}

			const emulator_object<MEMORY_IMAGE_INFORMATION> info{c.emu, memory_information};

			info.access([&](MEMORY_IMAGE_INFORMATION& image_info)
			{
				image_info.ImageBase = reinterpret_cast<void*>(mod->image_base);
				image_info.SizeOfImage = mod->size_of_image;
			});

			return STATUS_SUCCESS;
		}

		if (info_class == MemoryRegionInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(MEMORY_REGION_INFORMATION));
			}

			if (memory_information_length != sizeof(MEMORY_REGION_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const auto region_info = c.emu.get_region_info(base_address);
			if (!region_info.is_reserved)
			{
				return STATUS_INVALID_ADDRESS;
			}

			const emulator_object<MEMORY_REGION_INFORMATION> info{c.emu, memory_information};

			info.access([&](MEMORY_REGION_INFORMATION& image_info)
			{
				memset(&image_info, 0, sizeof(image_info));

				image_info.AllocationBase = reinterpret_cast<void*>(region_info.allocation_base);
				image_info.AllocationProtect = 0;
				image_info.PartitionId = 0;
				image_info.RegionSize = region_info.allocation_length;
				image_info.Reserved = 0x10;
			});

			return STATUS_SUCCESS;
		}

		printf("Unsupported memory info class: %X\n", info_class);
		c.emu.stop();
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, const uint32_t info_class,
	                                         const uint64_t system_information,
	                                         const uint32_t system_information_length,
	                                         const emulator_object<uint32_t> return_length)
	{
		if (info_class == SystemFlushInformation
			|| info_class == SystemHypervisorSharedPageInformation
		)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == SystemTimeOfDayInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(SYSTEM_TIMEOFDAY_INFORMATION));
			}

			if (system_information_length != sizeof(SYSTEM_TIMEOFDAY_INFORMATION))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const emulator_object<SYSTEM_TIMEOFDAY_INFORMATION> info_obj{c.emu, system_information};

			info_obj.access([&](SYSTEM_TIMEOFDAY_INFORMATION& info)
			{
				info.BootTime.QuadPart = 0;
				// TODO: Fill
			});

			return STATUS_SUCCESS;
		}

		if (info_class == SystemRangeStartInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(SYSTEM_RANGE_START_INFORMATION));
			}

			if (system_information_length != sizeof(SYSTEM_RANGE_START_INFORMATION))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const emulator_object<SYSTEM_RANGE_START_INFORMATION> info_obj{c.emu, system_information};

			info_obj.access([&](SYSTEM_RANGE_START_INFORMATION& info)
			{
				info.SystemRangeStart = 0xFFFF800000000000;
			});

			return STATUS_SUCCESS;
		}

		if (info_class == SystemProcessorInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(SYSTEM_PROCESSOR_INFORMATION));
			}

			if (system_information_length != sizeof(SYSTEM_PROCESSOR_INFORMATION))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const emulator_object<SYSTEM_PROCESSOR_INFORMATION> info_obj{c.emu, system_information};

			info_obj.access([&](SYSTEM_PROCESSOR_INFORMATION& info)
			{
				memset(&info, 0, sizeof(info));
				info.MaximumProcessors = 2;
				info.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
			});

			return STATUS_SUCCESS;
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

		if (info_class == SystemErrorPortTimeouts)
		{
			if (return_length)
			{
				return_length.write(sizeof(SYSTEM_ERROR_PORT_TIMEOUTS));
			}

			if (system_information_length != sizeof(SYSTEM_ERROR_PORT_TIMEOUTS))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const emulator_object<SYSTEM_ERROR_PORT_TIMEOUTS> info_obj{c.emu, system_information};

			info_obj.access([&](SYSTEM_ERROR_PORT_TIMEOUTS& info)
			{
				info.StartTimeout = 0;
				info.CommTimeout = 0;
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

	NTSTATUS handle_NtDuplicateObject(const syscall_context& /*c*/, uint64_t source_process_handle,
	                                  uint64_t source_handle, uint64_t target_process_handle,
	                                  const emulator_object<handle> target_handle,
	                                  const ACCESS_MASK /*desired_access*/, const ULONG /*handle_attributes*/,
	                                  const ULONG /*options*/)
	{
		if (source_process_handle != ~0ULL || target_process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		handle source{};

		source.bits = source_handle;
		if (source.value.is_pseudo)
		{
			target_handle.write(source);
			return STATUS_SUCCESS;
		}

		puts("Duplicating non-pseudo object not supported yet!");
		return STATUS_NOT_SUPPORTED;
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
			|| info_class == SystemSupportedProcessorArchitectures2
			|| info_class == SystemFeatureConfigurationSectionInformation)
		{
			//printf("Unsupported, but allowed system info class: %X\n", info_class);
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

	NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, const uint64_t process_handle,
	                                          const uint32_t info_class, const uint64_t process_information,
	                                          const uint32_t process_information_length,
	                                          const emulator_object<uint32_t> return_length)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == ProcessImageInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(SECTION_IMAGE_INFORMATION));
			}

			if (process_information_length != sizeof(SECTION_IMAGE_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<SECTION_IMAGE_INFORMATION> info{c.emu, process_information};
			info.access([&](SECTION_IMAGE_INFORMATION& i)
			{
				const auto& mod = *c.proc.executable;

				const emulator_object<IMAGE_DOS_HEADER> dos_header_obj{c.emu, mod.image_base};
				const auto dos_header = dos_header_obj.read();

				const emulator_object<IMAGE_NT_HEADERS> nt_headers_obj{c.emu, mod.image_base + dos_header.e_lfanew};
				const auto nt_headers = nt_headers_obj.read();

				const auto& file_header = nt_headers.FileHeader;
				const auto& optional_header = nt_headers.OptionalHeader;

				i.TransferAddress = nullptr;
				i.MaximumStackSize = optional_header.SizeOfStackReserve;
				i.CommittedStackSize = optional_header.SizeOfStackCommit;
				i.SubSystemType = optional_header.Subsystem;
				i.SubSystemMajorVersion = optional_header.MajorSubsystemVersion;
				i.SubSystemMinorVersion = optional_header.MinorSubsystemVersion;
				i.MajorOperatingSystemVersion = optional_header.MajorOperatingSystemVersion;
				i.MinorOperatingSystemVersion = optional_header.MinorOperatingSystemVersion;
				i.ImageCharacteristics = file_header.Characteristics;
				i.DllCharacteristics = optional_header.DllCharacteristics;
				i.Machine = file_header.Machine;
				i.ImageContainsCode = TRUE;
				i.ImageFlags = 0; // TODO
				i.ImageFileSize = optional_header.SizeOfImage;
				i.LoaderFlags = optional_header.LoaderFlags;
				i.CheckSum = optional_header.CheckSum;
			});

			return STATUS_SUCCESS;
		}

		if (info_class == ProcessCookie)
		{
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

		if (info_class == ProcessDebugPort)
		{
			if (return_length)
			{
				return_length.write(sizeof(DWORD_PTR));
			}

			if (process_information_length != sizeof(DWORD_PTR))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<DWORD_PTR> info{c.emu, process_information};
			info.write(0);

			return STATUS_SUCCESS;
		}

		if (info_class == ProcessDefaultHardErrorMode)
		{
			if (return_length)
			{
				return_length.write(sizeof(ULONG));
			}

			if (process_information_length != sizeof(ULONG))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<ULONG> info{c.emu, process_information};
			info.write(0);

			return STATUS_SUCCESS;
		}

		if (info_class == ProcessEnclaveInformation
			|| info_class == ProcessMitigationPolicy)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == ProcessBasicInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(PROCESS_BASIC_INFORMATION));
			}

			if (process_information_length != sizeof(PROCESS_BASIC_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<PROCESS_BASIC_INFORMATION> info{c.emu, process_information};
			info.access([&](PROCESS_BASIC_INFORMATION& basic_info)
			{
				basic_info.PebBaseAddress = c.proc.peb.ptr();
				basic_info.UniqueProcessId = reinterpret_cast<HANDLE>(1);
			});

			return STATUS_SUCCESS;
		}

		if (info_class == ProcessImageFileNameWin32)
		{
			const auto peb = c.proc.peb.read();
			emulator_object<RTL_USER_PROCESS_PARAMETERS> proc_params{c.emu, peb.ProcessParameters};
			const auto params = proc_params.read();
			const auto length = params.ImagePathName.Length + sizeof(UNICODE_STRING) + 2;

			if (return_length)
			{
				return_length.write(static_cast<uint32_t>(length));
			}

			if (process_information_length < length)
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<UNICODE_STRING> info{c.emu, process_information};
			info.access([&](UNICODE_STRING& str)
			{
				const auto buffer_start = static_cast<uint64_t>(process_information) + sizeof(UNICODE_STRING);
				const auto string = read_unicode_string(c.emu, params.ImagePathName);
				c.emu.write_memory(buffer_start, string.c_str(), (string.size() + 1) * 2);
				str.Length = params.ImagePathName.Length;
				str.MaximumLength = str.Length;
				str.Buffer = reinterpret_cast<wchar_t*>(buffer_start);
			});

			return STATUS_SUCCESS;
		}

		printf("Unsupported process info class: %X\n", info_class);
		c.emu.stop();

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, const uint64_t thread_handle,
	                                         const uint32_t info_class, const uint64_t thread_information,
	                                         const uint32_t thread_information_length,
	                                         const emulator_object<uint32_t> return_length)
	{
		if (thread_handle != ~1ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == ThreadBasicInformation)
		{
			if (return_length)
			{
				return_length.write(sizeof(THREAD_BASIC_INFORMATION));
			}

			if (thread_information_length != sizeof(THREAD_BASIC_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<THREAD_BASIC_INFORMATION> info{c.emu, thread_information};
			info.access([&](THREAD_BASIC_INFORMATION& i)
			{
				i.TebBaseAddress = c.win_emu.current_thread().teb->ptr();
				i.ClientId = c.win_emu.current_thread().teb->read().ClientId;
			});

			return STATUS_SUCCESS;
		}

		if (info_class == ThreadAmILastThread)
		{
			if (return_length)
			{
				return_length.write(sizeof(ULONG));
			}

			if (thread_information_length != sizeof(ULONG))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<ULONG> info{c.emu, thread_information};
			info.write(c.proc.threads.size() <= 1);

			return STATUS_SUCCESS;
		}

		printf("Unsupported thread info class: %X\n", info_class);
		c.emu.stop();

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtSetInformationFile(const syscall_context& c, const uint64_t file_handle,
	                                     const emulator_object<IO_STATUS_BLOCK> io_status_block,
	                                     const uint64_t file_information,
	                                     const ULONG length, const FILE_INFORMATION_CLASS info_class)
	{
		const auto* f = c.proc.files.get(file_handle);
		if (!f)
		{
			return STATUS_INVALID_HANDLE;
		}

		if (info_class == FilePositionInformation)
		{
			if (!f->handle)
			{
				return STATUS_NOT_SUPPORTED;
			}

			if (io_status_block)
			{
				IO_STATUS_BLOCK block{};
				block.Information = sizeof(FILE_POSITION_INFORMATION);
				io_status_block.write(block);
			}

			if (length != sizeof(FILE_POSITION_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<FILE_POSITION_INFORMATION> info{c.emu, file_information};
			const auto i = info.read();

			if (!f->handle.seek_to(i.CurrentByteOffset.QuadPart))
			{
				return STATUS_INVALID_PARAMETER;
			}

			return STATUS_SUCCESS;
		}

		printf("Unsupported set file info class: %X\n", info_class);
		c.emu.stop();

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQueryInformationFile(const syscall_context& c, uint64_t file_handle,
	                                       const emulator_object<IO_STATUS_BLOCK> io_status_block,
	                                       const uint64_t file_information,
	                                       const uint32_t length,
	                                       const uint32_t info_class)
	{
		const auto* f = c.proc.files.get(file_handle);
		if (!f)
		{
			return STATUS_INVALID_HANDLE;
		}

		if (info_class == FileStandardInformation)
		{
			if (io_status_block)
			{
				IO_STATUS_BLOCK block{};
				block.Information = sizeof(FILE_STANDARD_INFORMATION);
				io_status_block.write(block);
			}

			if (length != sizeof(FILE_STANDARD_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<FILE_STANDARD_INFORMATION> info{c.emu, file_information};
			FILE_STANDARD_INFORMATION i{};
			i.Directory = f->handle ? FALSE : TRUE;

			if (f->handle)
			{
				i.EndOfFile.QuadPart = f->handle.size();
			}

			info.write(i);

			return STATUS_SUCCESS;
		}

		if (info_class == FilePositionInformation)
		{
			if (!f->handle)
			{
				return STATUS_NOT_SUPPORTED;
			}

			if (io_status_block)
			{
				IO_STATUS_BLOCK block{};
				block.Information = sizeof(FILE_POSITION_INFORMATION);
				io_status_block.write(block);
			}

			if (length != sizeof(FILE_POSITION_INFORMATION))
			{
				return STATUS_BUFFER_OVERFLOW;
			}

			const emulator_object<FILE_POSITION_INFORMATION> info{c.emu, file_information};
			FILE_POSITION_INFORMATION i{};

			i.CurrentByteOffset.QuadPart = f->handle.tell();

			info.write(i);

			return STATUS_SUCCESS;
		}

		printf("Unsupported query file info class: %X\n", info_class);
		c.emu.stop();

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtSetInformationProcess(const syscall_context& c, const uint64_t process_handle,
	                                        const uint32_t info_class, const uint64_t /*process_information*/,
	                                        const uint32_t /*process_information_length*/)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == ProcessSchedulerSharedData
			|| info_class == ProcessTlsInformation
			|| info_class == ProcessConsoleHostProcess
			|| info_class == ProcessFaultInformation
			|| info_class == ProcessDefaultHardErrorMode
			|| info_class == ProcessRaiseUMExceptionOnInvalidHandleClose)
		{
			return STATUS_SUCCESS;
		}

		printf("Unsupported info process class: %X\n", info_class);
		c.emu.stop();

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtApphelpCacheControl()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, const uint64_t process_handle,
	                                       const emulator_object<uint64_t> base_address,
	                                       const emulator_object<uint32_t> bytes_to_protect,
	                                       const uint32_t protection,
	                                       const emulator_object<uint32_t> old_protection)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		const auto orig_start = base_address.read();
		const auto orig_length = bytes_to_protect.read();

		const auto aligned_start = page_align_down(orig_start);
		const auto aligned_length = page_align_up(orig_start + orig_length) - aligned_start;

		base_address.write(aligned_start);
		bytes_to_protect.write(static_cast<uint32_t>(aligned_length));

		const auto requested_protection = map_nt_to_emulator_protection(protection);

		c.win_emu.logger.print(color::dark_gray, "--> Changing protection at 0x%llX-0x%llX to %s\n", aligned_start,
		                       aligned_start + aligned_length, get_permission_string(requested_protection).c_str());

		memory_permission old_protection_value{};

		try
		{
			c.emu.protect_memory(aligned_start, aligned_length, requested_protection, &old_protection_value);
		}
		catch (...)
		{
			return STATUS_INVALID_ADDRESS;
		}

		const auto current_protection = map_emulator_to_nt_protection(old_protection_value);
		old_protection.write(current_protection);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtOpenDirectoryObject(const syscall_context& c,
	                                      const emulator_object<uint64_t> directory_handle,
	                                      const ACCESS_MASK /*desired_access*/,
	                                      const emulator_object<OBJECT_ATTRIBUTES> object_attributes)
	{
		const auto attributes = object_attributes.read();
		const auto object_name = read_unicode_string(c.emu, attributes.ObjectName);

		if (object_name == L"\\KnownDlls")
		{
			directory_handle.write(KNOWN_DLLS_DIRECTORY.bits);
			return STATUS_SUCCESS;
		}

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtOpenSymbolicLinkObject(const syscall_context& c, const emulator_object<uint64_t> link_handle,
	                                         ACCESS_MASK /*desired_access*/,
	                                         const emulator_object<OBJECT_ATTRIBUTES> object_attributes)
	{
		const auto attributes = object_attributes.read();
		const auto object_name = read_unicode_string(c.emu, attributes.ObjectName);

		if (object_name == L"KnownDllPath")
		{
			link_handle.write(KNOWN_DLLS_SYMLINK.bits);
			return STATUS_SUCCESS;
		}

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS WINAPI handle_NtQuerySymbolicLinkObject(const syscall_context& c, const uint64_t link_handle,
	                                                 const emulator_object<UNICODE_STRING> link_target,
	                                                 const emulator_object<ULONG> returned_length)
	{
		if (link_handle == KNOWN_DLLS_SYMLINK)
		{
			constexpr std::wstring_view system32 = L"C:\\WINDOWS\\System32";
			constexpr auto str_length = system32.size() * 2;
			constexpr auto max_length = str_length + 2;

			returned_length.write(max_length);

			bool too_small = false;
			link_target.access([&](UNICODE_STRING& str)
			{
				if (str.MaximumLength < max_length)
				{
					too_small = true;
					return;
				}

				str.Length = str_length;
				c.emu.write_memory(reinterpret_cast<uint64_t>(str.Buffer), system32.data(), max_length);
			});

			return too_small
				       ? STATUS_BUFFER_TOO_SMALL
				       : STATUS_SUCCESS;
		}

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, const uint64_t process_handle,
	                                          const emulator_object<uint64_t> base_address,
	                                          const emulator_object<uint64_t> bytes_to_allocate,
	                                          const uint32_t allocation_type,
	                                          const uint32_t page_protection)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
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
			return STATUS_NOT_SUPPORTED;
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

	NTSTATUS handle_NtCreateSection(const syscall_context& c, const emulator_object<uint64_t> section_handle,
	                                const ACCESS_MASK /*desired_access*/,
	                                const emulator_object<OBJECT_ATTRIBUTES> /*object_attributes*/,
	                                const emulator_object<ULARGE_INTEGER> maximum_size,
	                                const ULONG /*section_page_protection*/, const ULONG /*allocation_attributes*/,
	                                const uint64_t /*file_handle*/)
	{
		//puts("NtCreateSection not supported");
		section_handle.write(SHARED_SECTION.bits);

		maximum_size.access([&c](ULARGE_INTEGER& large_int)
		{
			large_int.QuadPart = page_align_up(large_int.QuadPart);
			c.proc.shared_section_size = large_int.QuadPart;
		});

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtConnectPort(const syscall_context& c, const emulator_object<uint64_t> client_port_handle,
	                              const emulator_object<UNICODE_STRING> server_port_name,
	                              const emulator_object<SECURITY_QUALITY_OF_SERVICE> /*security_qos*/,
	                              const emulator_object<PORT_VIEW> client_shared_memory,
	                              const emulator_object<REMOTE_PORT_VIEW> /*server_shared_memory*/,
	                              const emulator_object<ULONG> /*maximum_message_length*/,
	                              uint64_t connection_info,
	                              const emulator_object<ULONG> connection_info_length)
	{
		auto port_name = read_unicode_string(c.emu, server_port_name);
		c.win_emu.logger.print(color::dark_gray, "NtConnectPort: %S\n", port_name.c_str());

		port p{};
		p.name = std::move(port_name);

		if (connection_info)
		{
			std::vector<uint8_t> zero_mem{};
			zero_mem.resize(connection_info_length.read(), 0);
			c.emu.write_memory(connection_info, zero_mem.data(), zero_mem.size());
		}

		client_shared_memory.access([&](PORT_VIEW& view)
		{
			p.view_base = c.emu.allocate_memory(view.ViewSize, memory_permission::read_write);
			view.ViewBase = reinterpret_cast<void*>(p.view_base);
			view.ViewRemoteBase = view.ViewBase;
		});

		const auto handle = c.proc.ports.store(std::move(p));
		client_port_handle.write(handle.bits);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, uint64_t process_handle, uint64_t base_address,
	                                    uint64_t buffer, ULONG number_of_bytes_to_read,
	                                    const emulator_object<ULONG> number_of_bytes_read)
	{
		number_of_bytes_read.write(0);

		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		std::vector<uint8_t> memory{};
		memory.resize(number_of_bytes_read);

		if (!c.emu.try_read_memory(base_address, memory.data(), memory.size()))
		{
			return STATUS_INVALID_ADDRESS;
		}

		c.emu.write_memory(buffer, memory.data(), memory.size());
		number_of_bytes_read.write(number_of_bytes_to_read);
		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtDeviceIoControlFile()
	{
		//puts("NtDeviceIoControlFile not supported");
		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtQueryWnfStateData()
	{
		//puts("NtQueryWnfStateData not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQueryWnfStateNameInformation()
	{
		//puts("NtQueryWnfStateNameInformation not supported");
		//return STATUS_NOT_SUPPORTED;
		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtOpenProcessToken()
	{
		//puts("NtOpenProcessToken not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQuerySecurityAttributesToken()
	{
		//puts("NtQuerySecurityAttributesToken not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQueryLicenseValue()
	{
		//puts("NtQueryLicenseValue not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtTestAlert()
	{
		//puts("NtTestAlert not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQueryInformationToken()
	{
		//puts("NtQueryInformationToken not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtDxgkIsFeatureEnabled()
	{
		//puts("NtDxgkIsFeatureEnabled not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtQueryInstallUILanguage()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtUserDisplayConfigGetDeviceInfo()
	{
		//puts("NtUserDisplayConfigGetDeviceInfo not supported");
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtGdiInit2(const syscall_context& c)
	{
		c.proc.peb.access([&](PEB& peb)
		{
			if (!peb.GdiSharedHandleTable)
			{
				peb.GdiSharedHandleTable = c.proc.base_allocator.reserve<GDI_SHARED_MEMORY>().ptr();
			}
		});

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtGetMUIRegistryInfo()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtUserRegisterWindowMessage()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtUserGetThreadState()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtIsUILanguageComitted()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtUpdateWnfStateData()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, const uint64_t port_handle,
	                                          const ULONG /*flags*/,
	                                          const emulator_object<PORT_MESSAGE> /*send_message*/,
	                                          const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
	                                          /*send_message_attributes*/
	                                          ,
	                                          const emulator_object<PORT_MESSAGE> receive_message,
	                                          const emulator_object<SIZE_T> /*buffer_length*/,
	                                          const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
	                                          /*receive_message_attributes*/,
	                                          const emulator_object<LARGE_INTEGER> /*timeout*/)
	{
		const auto* port = c.proc.ports.get(port_handle);
		if (!port)
		{
			return STATUS_INVALID_HANDLE;
		}

		if (port->name != L"\\Windows\\ApiPort")
		{
			puts("!!! BAD PORT");
			return STATUS_NOT_SUPPORTED;
		}

		const emulator_object<PORT_DATA_ENTRY> data{c.emu, receive_message.value() + 0x48};
		const auto dest = data.read();
		const auto base = reinterpret_cast<uint64_t>(dest.Base);

		const auto value = base + 0x10;
		c.emu.write_memory(base + 8, &value, sizeof(value));

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtInitializeNlsFiles(const syscall_context& c, const emulator_object<uint64_t> base_address,
	                                     const emulator_object<LCID> default_locale_id,
	                                     const emulator_object<LARGE_INTEGER> /*default_casing_table_size*/)
	{
		const auto locale_file = utils::io::read_file(R"(C:\Windows\System32\locale.nls)");
		if (locale_file.empty())
		{
			return STATUS_FILE_INVALID;
		}

		const auto size = page_align_up(locale_file.size());
		const auto base = c.emu.allocate_memory(size, memory_permission::read);
		c.emu.write_memory(base, locale_file.data(), locale_file.size());

		base_address.write(base);
		default_locale_id.write(0x407);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtContinue(const syscall_context& c, const emulator_object<CONTEXT> thread_context,
	                           const BOOLEAN /*raise_alert*/)
	{
		c.write_status = false;

		const auto context = thread_context.read();
		context_frame::restore(c.emu, context);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtTerminateProcess(const syscall_context& c, const uint64_t process_handle,
	                                   NTSTATUS exit_status)
	{
		if (process_handle == 0)
		{
			for (auto& t : c.proc.threads)
			{
				if (&t.second != c.proc.active_thread)
				{
					t.second.exit_status = exit_status;
				}
			}

			return STATUS_SUCCESS;
		}

		if (process_handle == ~0ULL)
		{
			c.proc.exit_status = exit_status;
			c.emu.stop();
			return STATUS_SUCCESS;
		}

		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtReadFile(const syscall_context& c, const handle file_handle, const uint64_t /*event*/,
	                           const uint64_t /*apc_routine*/,
	                           const uint64_t /*apc_context*/,
	                           const emulator_object<IO_STATUS_BLOCK> io_status_block,
	                           uint64_t buffer, const ULONG length,
	                           const emulator_object<LARGE_INTEGER> /*byte_offset*/,
	                           const emulator_object<ULONG> /*key*/)
	{
		const auto* f = c.proc.files.get(file_handle);
		if (!f)
		{
			return STATUS_INVALID_HANDLE;
		}

		std::string temp_buffer{};
		temp_buffer.resize(length);

		const auto bytes_read = fread(temp_buffer.data(), 1, temp_buffer.size(), f->handle);

		if (io_status_block)
		{
			IO_STATUS_BLOCK block{};
			block.Information = bytes_read;
			io_status_block.write(block);
		}

		c.emu.write_memory(buffer, temp_buffer.data(), temp_buffer.size());
		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtWriteFile(const syscall_context& c, const handle file_handle, const uint64_t /*event*/,
	                            const uint64_t /*apc_routine*/,
	                            const uint64_t /*apc_context*/,
	                            const emulator_object<IO_STATUS_BLOCK> io_status_block,
	                            uint64_t buffer, const ULONG length,
	                            const emulator_object<LARGE_INTEGER> /*byte_offset*/,
	                            const emulator_object<ULONG> /*key*/)
	{
		std::string temp_buffer{};
		temp_buffer.resize(length);
		c.emu.read_memory(buffer, temp_buffer.data(), temp_buffer.size());


		if (file_handle == STDOUT_HANDLE)
		{
			if (io_status_block)
			{
				IO_STATUS_BLOCK block{};
				block.Information = length;
				io_status_block.write(block);
			}

			c.win_emu.logger.info("%.*s", static_cast<int>(temp_buffer.size()), temp_buffer.data());

			return STATUS_SUCCESS;
		}

		const auto* f = c.proc.files.get(file_handle);
		if (!f)
		{
			return STATUS_INVALID_HANDLE;
		}

		const auto bytes_written = fwrite(temp_buffer.data(), 1, temp_buffer.size(), f->handle);

		if (io_status_block)
		{
			IO_STATUS_BLOCK block{};
			block.Information = bytes_written;
			io_status_block.write(block);
		}

		return STATUS_SUCCESS;
	}

	const wchar_t* map_mode(const ACCESS_MASK desired_access, const ULONG create_disposition)
	{
		const auto* mode = L"";

		switch (create_disposition)
		{
		case FILE_CREATE:
		case FILE_SUPERSEDE:
			if (desired_access & GENERIC_WRITE)
			{
				mode = L"wb";
			}
			break;

		case FILE_OPEN:
		case FILE_OPEN_IF:
			if (desired_access & GENERIC_WRITE)
			{
				mode = L"r+b";
			}
			else if (desired_access & GENERIC_READ)
			{
				mode = L"rb";
			}
			break;

		case FILE_OVERWRITE:
		case FILE_OVERWRITE_IF:
			if (desired_access & GENERIC_WRITE)
			{
				mode = L"w+b";
			}
			break;

		default:
			mode = L"";
			break;
		}

		if (desired_access & FILE_APPEND_DATA)
		{
			mode = L"a+b";
		}

		return mode;
	}

	NTSTATUS handle_NtCreateFile(const syscall_context& c, const emulator_object<uint64_t> file_handle,
	                             ACCESS_MASK desired_access,
	                             const emulator_object<OBJECT_ATTRIBUTES> object_attributes,
	                             const emulator_object<IO_STATUS_BLOCK> /*io_status_block*/,
	                             const emulator_object<LARGE_INTEGER> /*allocation_size*/, ULONG /*file_attributes*/,
	                             ULONG /*share_access*/, ULONG create_disposition, ULONG /*create_options*/,
	                             uint64_t /*ea_buffer*/,
	                             ULONG /*ea_length*/)
	{
		const auto attributes = object_attributes.read();
		auto filename = read_unicode_string(c.emu, attributes.ObjectName);

		if (filename == L"\\Device\\ConDrv\\Server")
		{
			file_handle.write(CONSOLE_SERVER.bits);
			return STATUS_SUCCESS;
		}

		if (filename == L"\\Device\\DeviceApi\\CMApi")
		{
			file_handle.write(CM_API.bits);
			return STATUS_SUCCESS;
		}

		if (filename == L"\\Device\\KsecDD")
		{
			file_handle.write(KSEC_DD.bits);
			return STATUS_SUCCESS;
		}

		if (filename.starts_with(L"\\Device\\"))
		{
			return STATUS_NOT_SUPPORTED;
		}

		handle root_handle{};
		root_handle.bits = reinterpret_cast<uint64_t>(attributes.RootDirectory);
		if (root_handle.value.is_pseudo && (filename == L"\\Reference" || filename == L"\\Connect"))
		{
			file_handle.write(root_handle.bits);
			return STATUS_SUCCESS;
		}

		file f{};
		f.name = std::move(filename);

		if (attributes.RootDirectory)
		{
			const auto* root = c.proc.files.get(reinterpret_cast<uint64_t>(attributes.RootDirectory));
			if (!root)
			{
				return STATUS_INVALID_HANDLE;
			}

			f.name = root->name + f.name;
		}

		if (f.name.ends_with(L"\\"))
		{
			c.win_emu.logger.print(color::dark_gray, "--> Opening folder: %S\n", f.name.c_str());

			const auto handle = c.proc.files.store(std::move(f));
			file_handle.write(handle.bits);

			return STATUS_SUCCESS;
		}

		c.win_emu.logger.print(color::dark_gray, "--> Opening file: %S\n", f.name.c_str());

		const auto* mode = map_mode(desired_access, create_disposition);

		FILE* file{};
		const auto error = _wfopen_s(&file, f.name.c_str(), mode);

		if (!file)
		{
			switch (error)
			{
			case ENOENT:
				return STATUS_OBJECT_NAME_NOT_FOUND;
			case EACCES:
				return STATUS_ACCESS_DENIED;
			case EISDIR:
				return STATUS_FILE_IS_A_DIRECTORY;
			default:
				return STATUS_NOT_SUPPORTED;
			}
		}

		f.handle = file;

		const auto handle = c.proc.files.store(std::move(f));
		file_handle.write(handle.bits);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtOpenFile(const syscall_context& c,
	                           const emulator_object<uint64_t> file_handle,
	                           const ACCESS_MASK desired_access,
	                           const emulator_object<OBJECT_ATTRIBUTES> object_attributes,
	                           const emulator_object<IO_STATUS_BLOCK> io_status_block,
	                           const ULONG share_access,
	                           const ULONG open_options)
	{
		return handle_NtCreateFile(c, file_handle, desired_access, object_attributes, io_status_block, {c.emu}, 0,
		                           share_access, FILE_OPEN, open_options, 0, 0);
	}

	NTSTATUS handle_NtQueryInformationJobObject()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtSetSystemInformation()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtRaiseHardError(const syscall_context& c, const NTSTATUS error_status,
	                                 const ULONG /*number_of_parameters*/,
	                                 const emulator_object<UNICODE_STRING> /*unicode_string_parameter_mask*/,
	                                 const emulator_object<DWORD> /*parameters*/,
	                                 const HARDERROR_RESPONSE_OPTION /*valid_response_option*/,
	                                 const emulator_object<HARDERROR_RESPONSE> response)
	{
		if (response)
		{
			response.write(ResponseAbort);
		}

		printf("Hard error: %X\n", static_cast<uint32_t>(error_status));
		c.proc.exception_rip = c.emu.read_instruction_pointer();
		c.emu.stop();

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtRaiseException(const syscall_context& c,
	                                 const emulator_object<EXCEPTION_RECORD> /*exception_record*/,
	                                 const emulator_object<CONTEXT> thread_context, BOOLEAN handle_exception)
	{
		if (handle_exception)
		{
			puts("Unhandled exceptions not supported yet!");
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		c.proc.exception_rip = thread_context.read().Rip;
		c.emu.stop();

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, const emulator_object<uint64_t> semaphore_handle,
	                                  const ACCESS_MASK /*desired_access*/,
	                                  const emulator_object<OBJECT_ATTRIBUTES> object_attributes,
	                                  const ULONG initial_count, const ULONG maximum_count)
	{
		semaphore s{};
		s.current_count = initial_count;
		s.max_count = maximum_count;

		if (object_attributes)
		{
			const auto attributes = object_attributes.read();
			if (attributes.ObjectName)
			{
				s.name = read_unicode_string(c.emu, attributes.ObjectName);
			}
		}

		const auto handle = c.proc.semaphores.store(std::move(s));
		semaphore_handle.write(handle.bits);

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtAddAtomEx(const syscall_context& c, const uint64_t atom_name, const ULONG length,
	                            const emulator_object<RTL_ATOM> atom, const ULONG /*flags*/)
	{
		std::wstring name{};
		name.resize(length / 2);

		c.emu.read_memory(atom_name, name.data(), length);

		uint16_t index = 0;
		if (!c.proc.atoms.empty())
		{
			auto i = c.proc.atoms.end();
			--i;
			index = i->first + 1;
		}

		std::optional<uint16_t> last_entry{};
		for (auto& entry : c.proc.atoms)
		{
			if (entry.second == name)
			{
				if (atom)
				{
					atom.write(entry.first);
					return STATUS_SUCCESS;
				}
			}

			if (entry.first > 0)
			{
				if (!last_entry)
				{
					index = 0;
				}
				else
				{
					const auto diff = entry.first - *last_entry;
					if (diff > 1)
					{
						index = *last_entry + 1;
					}
				}
			}

			last_entry = entry.first;
		}

		c.proc.atoms[index] = std::move(name);
		atom.write(index);
		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, const uint64_t process_handle,
	                                     const uint64_t base_address)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		const auto* mod = c.proc.module_manager.find_by_address(base_address);
		if (!mod)
		{
			puts("Unmapping non-module section not supported!");
		}
		else
		{
			printf("Unmapping section %s not supported!\n", mod->name.c_str());
		}

		c.emu.stop();
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtCreateThreadEx(const syscall_context& c, const emulator_object<uint64_t> thread_handle,
	                                 const ACCESS_MASK /*desired_access*/,
	                                 const emulator_object<OBJECT_ATTRIBUTES> /*object_attributes*/,
	                                 const uint64_t process_handle, const uint64_t start_routine,
	                                 const uint64_t argument, const ULONG /*create_flags*/, const SIZE_T /*zero_bits*/,
	                                 const SIZE_T stack_size, const SIZE_T /*maximum_stack_size*/,
	                                 const emulator_object<PS_ATTRIBUTE_LIST> attribute_list)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		const auto h = c.proc.create_thread(c.emu, start_routine, argument, stack_size);
		thread_handle.write(h.bits);

		if (!attribute_list)
		{
			return STATUS_SUCCESS;
		}

		const auto* thread = c.proc.threads.get(h);

		const emulator_object<PS_ATTRIBUTE> attributes{
			c.emu, attribute_list.value() + offsetof(PS_ATTRIBUTE_LIST, Attributes)
		};

		const auto total_length = attribute_list.read().TotalLength;

		constexpr auto entry_size = sizeof(PS_ATTRIBUTE);
		constexpr auto header_size = sizeof(PS_ATTRIBUTE_LIST) - entry_size;
		const auto attribute_count = (total_length - header_size) / entry_size;

		for (size_t i = 0; i < attribute_count; ++i)
		{
			attributes.access([&](const PS_ATTRIBUTE& attribute)
			{
				const auto type = attribute.Attribute & ~PS_ATTRIBUTE_THREAD;

				if (type == PsAttributeClientId)
				{
					const auto client_id = thread->teb->read().ClientId;
					write_attribute(c.emu, attribute, client_id);
				}
				else if (type == PsAttributeTebAddress)
				{
					write_attribute(c.emu, attribute, thread->teb->ptr());
				}
				else
				{
					printf("Unsupported thread attribute type: %llX\n", type);
				}
			}, i);
		}

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtQueryDebugFilterState()
	{
		return FALSE;
	}

	NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, const uint64_t handle_value,
	                                      const BOOLEAN alertable,
	                                      const emulator_object<LARGE_INTEGER> timeout)
	{
		if (alertable)
		{
			puts("Alertable NtWaitForSingleObject not supported yet!");
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		handle h{};
		h.bits = handle_value;

		if (h.value.type != handle_types::thread && h.value.type != handle_types::event)
		{
			puts("Unsupported handle type for NtWaitForSingleObject!");
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		auto& t = c.win_emu.current_thread();
		t.await_object = h;

		if (timeout.value() && !t.await_time.has_value())
		{
			t.await_time = convert_delay_interval_to_time_point(timeout.read());
		}

		c.win_emu.switch_thread = true;
		c.emu.stop();

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtTerminateThread(const syscall_context& c, const uint64_t thread_handle,
	                                  const NTSTATUS exit_status)
	{
		auto* thread = !thread_handle
			               ? c.proc.active_thread
			               : c.proc.threads.get(thread_handle);

		if (!thread)
		{
			return STATUS_INVALID_HANDLE;
		}

		thread->exit_status = exit_status;
		if (thread == c.proc.active_thread)
		{
			c.win_emu.switch_thread = true;
			c.emu.stop();
		}

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtDelayExecution(const syscall_context& c, const BOOLEAN alertable,
	                                 const emulator_object<LARGE_INTEGER> delay_interval)
	{
		if (alertable)
		{
			puts("Alertable NtDelayExecution not supported yet!");
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		auto& t = c.win_emu.current_thread();
		t.await_time = convert_delay_interval_to_time_point(delay_interval.read());

		c.win_emu.switch_thread = true;
		c.emu.stop();

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtAlertThreadByThreadId(const syscall_context& c, const uint64_t thread_id)
	{
		for (auto& t : c.proc.threads)
		{
			if (t.second.id == thread_id)
			{
				t.second.alerted = true;
				return STATUS_SUCCESS;
			}
		}

		return STATUS_INVALID_HANDLE;
	}

	NTSTATUS handle_NtAlertThreadByThreadIdEx(const syscall_context& c, const uint64_t thread_id,
	                                          const emulator_object<RTL_SRWLOCK> lock)
	{
		if (lock.value())
		{
			c.win_emu.logger.print(color::gray, "NtAlertThreadByThreadIdEx with lock not supported yet!");
			//c.emu.stop();
			//return STATUS_NOT_SUPPORTED;
		}

		return handle_NtAlertThreadByThreadId(c, thread_id);
	}

	NTSTATUS handle_NtWaitForAlertByThreadId(const syscall_context& c, const uint64_t,
	                                         const emulator_object<LARGE_INTEGER> timeout)
	{
		auto& t = c.win_emu.current_thread();
		t.waiting_for_alert = true;

		if (timeout.value() && !t.await_time.has_value())
		{
			t.await_time = convert_delay_interval_to_time_point(timeout.read());
		}

		c.win_emu.switch_thread = true;
		c.emu.stop();

		return STATUS_SUCCESS;
	}
}

void syscall_dispatcher::add_handlers(std::unordered_map<std::string, syscall_handler>& handler_mapping)
{
#define add_handler(syscall)                                                  \
	do                                                                        \
	{                                                                         \
		handler_mapping[#syscall] = make_syscall_handler<handle_##syscall>(); \
	} while(0)

	add_handler(NtSetInformationThread) ;
	add_handler(NtSetEvent);
	add_handler(NtClose);
	add_handler(NtOpenKey);
	add_handler(NtAllocateVirtualMemory);
	add_handler(NtQueryInformationProcess);
	add_handler(NtSetInformationProcess);
	add_handler(NtFreeVirtualMemory);
	add_handler(NtQueryVirtualMemory);
	add_handler(NtOpenThreadToken);
	add_handler(NtQueryPerformanceCounter);
	add_handler(NtQuerySystemInformation);
	add_handler(NtCreateEvent);
	add_handler(NtProtectVirtualMemory);
	add_handler(NtOpenDirectoryObject);
	add_handler(NtTraceEvent);
	add_handler(NtAllocateVirtualMemoryEx);
	add_handler(NtCreateIoCompletion);
	add_handler(NtCreateWaitCompletionPacket);
	add_handler(NtCreateWorkerFactory);
	add_handler(NtManageHotPatch);
	add_handler(NtOpenSection);
	add_handler(NtMapViewOfSection);
	add_handler(NtOpenSymbolicLinkObject);
	add_handler(NtQuerySymbolicLinkObject);
	add_handler(NtQuerySystemInformationEx);
	add_handler(NtOpenFile);
	add_handler(NtQueryVolumeInformationFile);
	add_handler(NtApphelpCacheControl);
	add_handler(NtCreateSection);
	add_handler(NtConnectPort);
	add_handler(NtCreateFile);
	add_handler(NtDeviceIoControlFile);
	add_handler(NtQueryWnfStateData);
	add_handler(NtOpenProcessToken);
	add_handler(NtQuerySecurityAttributesToken);
	add_handler(NtQueryLicenseValue);
	add_handler(NtTestAlert);
	add_handler(NtContinue);
	add_handler(NtTerminateProcess);
	add_handler(NtWriteFile);
	add_handler(NtRaiseHardError);
	add_handler(NtCreateSemaphore);
	add_handler(NtReadVirtualMemory);
	add_handler(NtQueryInformationToken);
	add_handler(NtDxgkIsFeatureEnabled);
	add_handler(NtAddAtomEx);
	add_handler(NtInitializeNlsFiles);
	add_handler(NtUnmapViewOfSection);
	add_handler(NtDuplicateObject);
	add_handler(NtQueryInformationThread);
	add_handler(NtQueryWnfStateNameInformation);
	add_handler(NtAlpcSendWaitReceivePort);
	add_handler(NtGdiInit2);
	add_handler(NtUserGetThreadState);
	add_handler(NtOpenKeyEx);
	add_handler(NtUserDisplayConfigGetDeviceInfo);
	add_handler(NtOpenEvent);
	add_handler(NtGetMUIRegistryInfo);
	add_handler(NtIsUILanguageComitted);
	add_handler(NtQueryInstallUILanguage);
	add_handler(NtUpdateWnfStateData);
	add_handler(NtRaiseException);
	add_handler(NtQueryInformationJobObject);
	add_handler(NtSetSystemInformation);
	add_handler(NtQueryInformationFile);
	add_handler(NtCreateThreadEx);
	add_handler(NtQueryDebugFilterState);
	add_handler(NtWaitForSingleObject);
	add_handler(NtTerminateThread);
	add_handler(NtDelayExecution);
	add_handler(NtWaitForAlertByThreadId);
	add_handler(NtAlertThreadByThreadIdEx);
	add_handler(NtAlertThreadByThreadId);
	add_handler(NtReadFile);
	add_handler(NtSetInformationFile);
	add_handler(NtUserRegisterWindowMessage);

#undef add_handler
}
