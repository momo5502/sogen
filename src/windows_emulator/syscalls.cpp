#include "std_include.hpp"
#include "syscalls.hpp"
#include "module_mapper.hpp"

struct syscall_context
{
	x64_emulator& emu;
	process_context& proc;
};

namespace
{
	constexpr uint64_t PSEUDO_BIT = 1ULL << 63ULL;
	constexpr uint64_t EVENT_BIT = 1ULL << 62ULL;
	constexpr uint64_t DIRECTORY_BIT = 1ULL << 61ULL;
	constexpr uint64_t SYMLINK_BIT = 1ULL << 60ULL;
	constexpr uint64_t FILE_BIT = 1ULL << 59ULL;

	constexpr uint64_t KNOWN_DLLS_DIRECTORY = DIRECTORY_BIT | PSEUDO_BIT | 0x1337;
	constexpr uint64_t KNOWN_DLLS_SYMLINK = SYMLINK_BIT | PSEUDO_BIT | 0x1337;

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


	bool is_uppercase(const char character)
	{
		return toupper(character) == character;
	}

	bool is_syscall(const std::string_view name)
	{
		return name.starts_with("Nt") && name.size() > 3 && is_uppercase(name[2]);
	}

	std::vector<std::string> find_syscalls(const exported_symbols& exports)
	{
		std::map<uint64_t, std::string> ordered_syscalls{};

		for (const auto& symbol : exports)
		{
			if (is_syscall(symbol.name))
			{
				ordered_syscalls[symbol.address] = symbol.name;
			}
		}

		std::vector<std::string> syscalls{};
		syscalls.reserve(ordered_syscalls.size());

		for (auto& syscall : ordered_syscalls)
		{
			syscalls.push_back(std::move(syscall.second));
		}

		return syscalls;
	}

	uint64_t get_syscall_id(const std::vector<std::string>& syscalls, const std::string_view name)
	{
		for (size_t i = 0; i < syscalls.size(); ++i)
		{
			if (syscalls[i] == name)
			{
				return i;
			}
		}

		throw std::runtime_error("Unable to determine syscall id: " + std::string(name));
	}

	uint32_t store_os_handle(process_context& proc, const HANDLE handle)
	{
		uint32_t index = 1;
		for (;; ++index)
		{
			if (!proc.os_handles.contains(index))
			{
				break;
			}
		}

		proc.os_handles[index] = handle;
		return index;
	}

	std::optional<HANDLE> get_os_handle(process_context& proc, const uint32_t handle)
	{
		const auto entry = proc.os_handles.find(handle);
		if (entry == proc.os_handles.end())
		{
			return {};
		}

		return entry->second;
	}

	std::optional<HANDLE> remove_os_handle(process_context& proc, const uint32_t handle)
	{
		const auto entry = proc.os_handles.find(handle);
		if (entry == proc.os_handles.end())
		{
			return {};
		}

		const auto res = entry->second;
		proc.os_handles.erase(entry);

		return res;
	}

	std::wstring read_unicode_string(emulator& emu, const emulator_object<UNICODE_STRING> uc_string)
	{
		static_assert(offsetof(UNICODE_STRING, Length) == 0);
		static_assert(offsetof(UNICODE_STRING, MaximumLength) == 2);
		static_assert(offsetof(UNICODE_STRING, Buffer) == 8);
		static_assert(sizeof(UNICODE_STRING) == 16);

		const auto ucs = uc_string.read();

		std::wstring result{};
		result.resize(ucs.Length / 2);

		emu.read_memory(reinterpret_cast<uint64_t>(ucs.Buffer), result.data(), ucs.Length);

		return result;
	}

	std::wstring read_unicode_string(emulator& emu, const PUNICODE_STRING uc_string)
	{
		return read_unicode_string(emu, emulator_object<UNICODE_STRING>{emu, uc_string});
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

	NTSTATUS handle_NtSetInformationThread(const syscall_context& c, const uint64_t /*thread_handle*/,
	                                       const THREADINFOCLASS info_class,
	                                       const uint64_t /*thread_information*/,
	                                       const uint32_t /*thread_information_length*/)
	{
		if (info_class == ThreadSchedulerSharedDataSlot)
		{
			return STATUS_SUCCESS;
		}

		printf("Unsupported thread info class: %X\n", info_class);
		c.emu.stop();
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
		if (handle & PSEUDO_BIT)
		{
			return STATUS_SUCCESS;
		}

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

		if (handle & FILE_BIT)
		{
			const auto event_index = static_cast<uint32_t>(handle & ~FILE_BIT);
			const auto entry = c.proc.files.find(event_index);
			if (entry != c.proc.files.end())
			{
				c.proc.files.erase(entry);
				return STATUS_SUCCESS;
			}
		}

		return STATUS_INVALID_HANDLE;
	}

	NTSTATUS handle_NtTraceEvent()
	{
		return STATUS_NOT_SUPPORTED;
	}

	NTSTATUS handle_NtOpenThreadToken()
	{
		return STATUS_NO_TOKEN;
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

	NTSTATUS handle_NtQueryVolumeInformationFile(const syscall_context& c, uint64_t /*file_handle*/,
	                                             uint64_t /*io_status_block*/, uint64_t fs_information, ULONG /*length*/,
	                                             FS_INFORMATION_CLASS fs_information_class)
	{
		if (fs_information_class != FileFsDeviceInformation)
		{
			printf("Unsupported process info class: %X\n", fs_information_class);
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
		}

		const emulator_object<FILE_FS_DEVICE_INFORMATION> info_obj{c.emu, fs_information};
		info_obj.access([&](FILE_FS_DEVICE_INFORMATION& info)
		{
			info.DeviceType = FILE_DEVICE_DISK;
			info.Characteristics = 0x20020;
		});

		return STATUS_SUCCESS;
	}

	NTSTATUS handle_NtOpenFile(const syscall_context& c,
	                           const emulator_object<uint64_t> file_handle,
	                           const ACCESS_MASK /*desired_access*/,
	                           const emulator_object<OBJECT_ATTRIBUTES> object_attributes,
	                           const emulator_object<IO_STATUS_BLOCK> /*io_status_block*/,
	                           const ULONG /*share_access*/,
	                           const ULONG /*open_options*/)
	{
		uint32_t index = 1;
		for (;; ++index)
		{
			if (!c.proc.files.contains(index))
			{
				break;
			}
		}

		file_handle.write(index | FILE_BIT);

		auto status = STATUS_SUCCESS;
		object_attributes.access([&](const OBJECT_ATTRIBUTES& attributes)
		{
			auto section = read_unicode_string(c.emu, attributes.ObjectName);
			if (!std::filesystem::exists(section))
			{
				status = STATUS_FILE_INVALID;
			}
			else
			{
				c.proc.files.try_emplace(index, std::move(section));
			}
		});

		return status;
	}

	NTSTATUS handle_NtOpenSection(const syscall_context& c, const emulator_object<uint64_t> section_handle,
	                              const ACCESS_MASK /*desired_access*/,
	                              const emulator_object<OBJECT_ATTRIBUTES> object_attributes)
	{
		uint32_t index = 1;
		for (;; ++index)
		{
			if (!c.proc.files.contains(index))
			{
				break;
			}
		}

		section_handle.write(index | FILE_BIT);

		auto status = STATUS_SUCCESS;
		std::wstring filename{};
		object_attributes.access([&](const OBJECT_ATTRIBUTES& attributes)
		{
			if (reinterpret_cast<uint64_t>(attributes.RootDirectory) != KNOWN_DLLS_DIRECTORY)
			{
				status = STATUS_NOT_SUPPORTED;
				return;
			}

			filename = read_unicode_string(c.emu, attributes.ObjectName);
			if (filename.starts_with(L"api-ms-"))
			{
				filename = L"C:\\WINDOWS\\System32\\downlevel\\" + filename;
			}
			else
			{
				filename = L"C:\\WINDOWS\\System32\\" + filename;
			}
		});

		if (status != STATUS_SUCCESS)
		{
			return status;
		}

		if (!std::filesystem::exists(filename))
		{
			return STATUS_FILE_INVALID;
		}

		c.proc.files.try_emplace(index, std::move(filename));

		return status;
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

		if (!(section_handle & FILE_BIT))
		{
			return STATUS_INVALID_HANDLE;
		}

		const auto section_index = static_cast<uint32_t>(section_handle & ~FILE_BIT);
		const auto section_entry = c.proc.files.find(section_index);
		if (section_entry == c.proc.files.end())
		{
			return STATUS_INVALID_HANDLE;
		}

		const auto& section_name = section_entry->second;
		const auto binary = map_file(c.emu, section_name);
		if (!binary.has_value())
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
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == MemoryWorkingSetExInformation
			|| info_class == MemoryImageExtensionInformation)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class != MemoryImageInformation)
		{
			printf("Unsupported memory info class: %X\n", info_class);
			c.emu.stop();
			return STATUS_NOT_SUPPORTED;
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
			return STATUS_NOT_SUPPORTED;
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

	NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, const uint64_t process_handle,
	                                          const uint32_t info_class, const uint64_t process_information,
	                                          const uint32_t process_information_length,
	                                          const emulator_object<uint32_t> return_length)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class != ProcessCookie)
		{
			printf("Unsupported process info class: %X\n", info_class);
			c.emu.stop();

			return STATUS_NOT_SUPPORTED;
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

	NTSTATUS handle_NtSetInformationProcess(const syscall_context& c, const uint64_t process_handle,
	                                        const uint32_t info_class, const uint64_t /*process_information*/,
	                                        const uint32_t /*process_information_length*/)
	{
		if (process_handle != ~0ULL)
		{
			return STATUS_NOT_SUPPORTED;
		}

		if (info_class == ProcessSchedulerSharedData)
		{
			return STATUS_SUCCESS;
		}

		printf("Unsupported process info class: %X\n", info_class);
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

		printf("Changing protection at %llX-%llX to %s\n", aligned_start, aligned_start + aligned_length,
		       get_permission_string(requested_protection).c_str());

		memory_permission old_protection_value{};
		c.emu.protect_memory(aligned_start, aligned_length, requested_protection, &old_protection_value);

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
			directory_handle.write(KNOWN_DLLS_DIRECTORY);
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
			link_handle.write(KNOWN_DLLS_SYMLINK);
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
}

syscall_dispatcher::syscall_dispatcher(const exported_symbols& ntdll_exports)
{
#define add_handler(syscall) do                             \
	{                                                       \
		const auto id = get_syscall_id(syscalls, #syscall); \
		auto handler = +[](const syscall_context& c)        \
		{                                                   \
			forward(c, handle_ ## syscall);                 \
		};                                                  \
		this->handlers_[id] = handler;                      \
	} while(0)

	const auto syscalls = find_syscalls(ntdll_exports);

	add_handler(NtSetInformationThread);
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

#undef add_handler
}

void syscall_dispatcher::dispatch(x64_emulator& emu, process_context& context)
{
	const auto address = emu.read_instruction_pointer();
	const auto syscall_id = emu.reg<uint32_t>(x64_register::eax);

	printf("Handling syscall: %X (%llX)\n", syscall_id, address);

	const syscall_context c{emu, context};

	try
	{
		const auto entry = this->handlers_.find(syscall_id);
		if (entry == this->handlers_.end())
		{
			printf("Unhandled syscall: %X\n", syscall_id);
			c.emu.reg<uint64_t>(x64_register::rax, STATUS_NOT_SUPPORTED);
			c.emu.stop();
		}
		else
		{
			entry->second(c);
		}
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
