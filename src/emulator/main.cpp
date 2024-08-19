#include "std_include.hpp"

#define GS_SEGMENT_ADDR 0x6000000ULL
#define GS_SEGMENT_SIZE (20 << 20)  // 20 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_ADDRESS 0x7ffffffde000
#define STACK_SIZE 0x40000

#define KUSD_ADDRESS 0x7ffe0000

#include "unicorn.hpp"
#include <utils/finally.hpp>

namespace
{
	bool is_within_start_and_end(const uint64_t value, const uint64_t start, const uint64_t end)
	{
		return value >= start && value < end;
	}

	bool is_within_start_and_length(const uint64_t value, const uint64_t start, const uint64_t length)
	{
		return is_within_start_and_end(value, start, start + length);
	}

	uint64_t align_down(const uint64_t value, const uint64_t alignment)
	{
		return value & ~(alignment - 1);
	}

	uint64_t align_up(const uint64_t value, const uint64_t alignment)
	{
		return align_down(value + (alignment - 1), alignment);
	}

	uint64_t page_align_down(const uint64_t value)
	{
		return align_down(value, 0x1000);
	}

	uint64_t page_align_up(const uint64_t value)
	{
		return align_up(value, 0x1000);
	}

	template <typename T>
	class unicorn_object
	{
	public:
		unicorn_object() = default;

		unicorn_object(const unicorn& uc, uint64_t address)
			: uc_(&uc)
			  , address_(address)
		{
		}

		uint64_t value() const
		{
			return this->address_;
		}

		uint64_t size() const
		{
			return sizeof(T);
		}

		uint64_t end() const
		{
			return this->value() + this->size();
		}

		T* ptr() const
		{
			return reinterpret_cast<T*>(this->address_);
		}

		operator bool() const
		{
			return this->address_ != 0;
		}

		T read() const
		{
			T obj{};

			e(uc_mem_read(*this->uc_, this->address_, &obj, sizeof(obj)));

			return obj;
		}

		void write(const T& value) const
		{
			e(uc_mem_write(*this->uc_, this->address_, &value, sizeof(value)));
		}

		template <typename F>
		void access(const F& accessor) const
		{
			T obj{};
			e(uc_mem_read(*this->uc_, this->address_, &obj, sizeof(obj)));

			accessor(obj);

			this->write(obj);
		}

	private:
		const unicorn* uc_{};
		uint64_t address_{};
	};

	class unicorn_allocator
	{
	public:
		unicorn_allocator() = default;

		unicorn_allocator(const unicorn& uc, const uint64_t address, const uint64_t size)
			: uc_(&uc)
			  , address_(address)
			  , size_(size)
			  , active_address_(address)
		{
		}

		uint64_t reserve(const uint64_t count, const uint64_t alignment = 1)
		{
			const auto potential_start = align_up(this->active_address_, alignment);
			const auto potential_end = potential_start + count;
			const auto total_end = this->address_ + this->size_;

			if (potential_end > total_end)
			{
				throw std::runtime_error("Out of memory");
			}

			this->active_address_ = potential_end;

			return potential_start;
		}

		template <typename T>
		unicorn_object<T> reserve()
		{
			const auto potential_start = this->reserve(sizeof(T), alignof(T));
			return unicorn_object<T>(*this->uc_, potential_start);
		}

		void make_unicode_string(UNICODE_STRING& result, const std::wstring_view str)
		{
			constexpr auto element_size = sizeof(str[0]);
			constexpr auto required_alignment = alignof(decltype(str[0]));
			const auto total_length = str.size() * element_size;

			const auto string_buffer = this->reserve(total_length, required_alignment);

			e(uc_mem_write(*this->uc_, string_buffer, str.data(), total_length));

			result.Buffer = reinterpret_cast<PWCH>(string_buffer);
			result.Length = static_cast<USHORT>(total_length);
			result.MaximumLength = result.Length;
		}

		unicorn_object<UNICODE_STRING> make_unicode_string(const std::wstring_view str)
		{
			const auto unicode_string = this->reserve<UNICODE_STRING>();

			unicode_string.access([&](UNICODE_STRING& unicode_str)
			{
				this->make_unicode_string(unicode_str, str);
			});

			return unicode_string;
		}

	private:
		const unicorn* uc_{};
		uint64_t address_{};
		uint64_t size_{};
		uint64_t active_address_{0};
	};

	class unicorn_hook
	{
	public:
		using function = std::function<void(const unicorn& uc, uint64_t address, uint32_t size)>;

		template <typename... Args>
		unicorn_hook(const unicorn& uc, const int type, const uint64_t begin, const uint64_t end, function callback,
		             Args... args)
			: uc_(&uc)
		{
			this->function_ = std::make_unique<internal_function>(
				[c = std::move(callback), &uc](const uint64_t address, const uint32_t size)
				{
					c(uc, address, size);
				});

			void* handler = +[](uc_engine*, const uint64_t address, const uint32_t size,
			                    void* user_data)
			{
				(*static_cast<internal_function*>(user_data))(address, size);
			};

			if (type == UC_HOOK_INSN)
			{
				handler = +[](uc_engine* uc, void* user_data)
				{
					uint64_t rip{};
					uc_reg_read(uc, UC_X86_REG_RIP, &rip);
					(*static_cast<internal_function*>(user_data))(rip, 0);
				};
			}

			if (type == UC_HOOK_MEM_READ)
			{
				handler = +[](uc_engine*, const uc_mem_type /*type*/, const uint64_t address, const int size,
				              const int64_t /*value*/, void* user_data)
				{
					(*static_cast<internal_function*>(user_data))(address, size);
				};
			}
			e(uc_hook_add(*this->uc_, &this->hook_, type, handler, this->function_.get(), begin, end, args...));
		}

		unicorn_hook(const unicorn_hook&) = delete;
		unicorn_hook& operator=(const unicorn_hook&) = delete;

		unicorn_hook(unicorn_hook&& obj) noexcept
		{
			this->operator=(std::move(obj));
		}

		unicorn_hook& operator=(unicorn_hook&& obj) noexcept
		{
			if (this != &obj)
			{
				this->remove();

				this->uc_ = obj.uc_;
				this->hook_ = obj.hook_;
				this->function_ = std::move(obj.function_);

				obj.hook_ = {};
			}

			return *this;
		}

		~unicorn_hook()
		{
			this->remove();
		}

		void remove()
		{
			if (this->hook_)
			{
				uc_hook_del(*this->uc_, this->hook_);
				this->hook_ = {};
			}

			this->function_ = {};
		}

	private:
		using internal_function = std::function<void(uint64_t address, uint32_t size)>;

		const unicorn* uc_{};
		uc_hook hook_{};
		std::unique_ptr<internal_function> function_{};
	};

	void setup_stack(const unicorn& uc, uint64_t stack_base, size_t stack_size)
	{
		e(uc_mem_map(uc, stack_base, stack_size, UC_PROT_READ | UC_PROT_WRITE));

		const uint64_t stack_end = stack_base + stack_size;
		e(uc_reg_write(uc, UC_X86_REG_RSP, &stack_end));
	}

	unicorn_allocator setup_gs_segment(const unicorn& uc, const uint64_t segment_base, const uint64_t size)
	{
		const std::array<uint64_t, 2> value = {
			IA32_GS_BASE_MSR,
			segment_base
		};

		e(uc_reg_write(uc, UC_X86_REG_MSR, value.data()));
		e(uc_mem_map(uc, segment_base, size, UC_PROT_READ | UC_PROT_WRITE));

		return {uc, segment_base, size};
	}

	void setup_kusd(const unicorn& uc)
	{
		e(uc_mem_map(uc, KUSD_ADDRESS, page_align_up(sizeof(KUSER_SHARED_DATA)), UC_PROT_READ));

		const unicorn_object<KUSER_SHARED_DATA> kusd_object{uc, KUSD_ADDRESS};
		kusd_object.access([](KUSER_SHARED_DATA& kusd)
		{
			const auto& real_kusd = *reinterpret_cast<KUSER_SHARED_DATA*>(KUSD_ADDRESS);

			memcpy(&kusd, &real_kusd, sizeof(kusd));

			kusd.ImageNumberLow = IMAGE_FILE_MACHINE_I386;
			kusd.ImageNumberHigh = IMAGE_FILE_MACHINE_AMD64;

			memset(&kusd.ProcessorFeatures, 0, sizeof(kusd.ProcessorFeatures));

			// ...
		});
	}

	struct mapped_binary
	{
		uint64_t image_base{};
		uint64_t size_of_image{};
		std::unordered_map<std::string, uint64_t> exports{};
	};

	mapped_binary map_module(const unicorn& uc, const std::vector<uint8_t>& module_data,
	                         const std::string& name)
	{
		mapped_binary binary{};

		// TODO: Range checks
		auto* ptr = module_data.data();
		auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(ptr);
		auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(ptr + dos_header->e_lfanew);
		auto& optional_header = nt_headers->OptionalHeader;

		binary.image_base = optional_header.ImageBase;
		binary.size_of_image = optional_header.SizeOfImage;

		while (true)
		{
			const auto res = uc_mem_map(uc, binary.image_base, binary.size_of_image, UC_PROT_READ);
			if (res == UC_ERR_OK)
			{
				break;
			}

			binary.image_base += 0x10000;

			if (binary.image_base < optional_header.ImageBase || (optional_header.DllCharacteristics &
				IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) == 0)
			{
				throw std::runtime_error("Failed to map range");
			}
		}

		printf("Mapping %s at %llX\n", name.c_str(), binary.image_base);

		e(uc_mem_write(uc, binary.image_base, ptr, optional_header.SizeOfHeaders));

		const std::span sections(IMAGE_FIRST_SECTION(nt_headers), nt_headers->FileHeader.NumberOfSections);

		for (const auto& section : sections)
		{
			const auto target_ptr = binary.image_base + section.VirtualAddress;

			if (section.SizeOfRawData > 0)
			{
				const void* source_ptr = ptr + section.PointerToRawData;

				const auto size_of_data = std::min(section.SizeOfRawData, section.Misc.VirtualSize);
				e(uc_mem_write(uc, target_ptr, source_ptr, size_of_data));
			}
			uint32_t permissions = UC_PROT_NONE;

			if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				permissions |= UC_PROT_EXEC;
			}

			if (section.Characteristics & IMAGE_SCN_MEM_READ)
			{
				permissions |= UC_PROT_READ;
			}

			if (section.Characteristics & IMAGE_SCN_MEM_WRITE)
			{
				permissions |= UC_PROT_WRITE;
			}

			const auto size_of_section = page_align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize));

			e(uc_mem_protect(uc, target_ptr, size_of_section, permissions));
		}

		auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (export_directory_entry.VirtualAddress == 0 || export_directory_entry.Size == 0)
		{
			return {};
		}

		const auto* export_directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(ptr + export_directory_entry.
			VirtualAddress);

		//const auto function_count = export_directory->NumberOfFunctions;
		const auto names_count = export_directory->NumberOfNames;

		const auto* names = reinterpret_cast<const DWORD*>(ptr + export_directory->AddressOfNames);
		const auto* ordinals = reinterpret_cast<const WORD*>(ptr + export_directory->AddressOfNameOrdinals);
		const auto* functions = reinterpret_cast<const DWORD*>(ptr + export_directory->AddressOfFunctions);

		for (DWORD i = 0; i < names_count; i++)
		{
			const auto* function_name = reinterpret_cast<const char*>(ptr + names[i]);
			const auto function_rva = functions[ordinals[i]];
			const auto function_address = binary.image_base + function_rva;

			binary.exports[function_name] = function_address;
		}

		return binary;
	}

	struct event
	{
		bool signaled{};
		EVENT_TYPE type{};

		bool is_signaled()
		{
			const auto res = this->signaled;

			if (this->type == SynchronizationEvent)
			{
				this->signaled = false;
			}

			return res;
		}
	};

	struct process_context
	{
		unicorn_object<TEB> teb{};
		unicorn_object<PEB> peb{};
		unicorn_object<RTL_USER_PROCESS_PARAMETERS> process_params{};

		mapped_binary executable{};
		mapped_binary ntdll{};

		std::vector<event> events{};
		unicorn_allocator gs_segment{};
	};

	process_context setup_teb_and_peb(const unicorn& uc)
	{
		setup_stack(uc, STACK_ADDRESS, STACK_SIZE);
		process_context context{};

		context.gs_segment = setup_gs_segment(uc, GS_SEGMENT_ADDR, GS_SEGMENT_SIZE);

		auto& gs = context.gs_segment;

		context.teb = gs.reserve<TEB>();
		context.peb = gs.reserve<PEB>();
		//context.ldr = gs.reserve<PEB_LDR_DATA>();
		context.process_params = gs.reserve<RTL_USER_PROCESS_PARAMETERS>();

		context.teb.access([&](TEB& teb)
		{
			teb.NtTib.StackLimit = reinterpret_cast<void*>(STACK_ADDRESS);
			teb.NtTib.StackBase = reinterpret_cast<void*>((STACK_ADDRESS + STACK_SIZE));
			teb.NtTib.Self = &context.teb.ptr()->NtTib;
			teb.ProcessEnvironmentBlock = context.peb.ptr();
		});

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = nullptr;
			//peb.Ldr = context.ldr.ptr();
			peb.ProcessHeap = nullptr;
			peb.ProcessHeaps = nullptr;
			peb.ProcessParameters = context.process_params.ptr();
		});

		context.process_params.access([&](RTL_USER_PROCESS_PARAMETERS& proc_params)
		{
			proc_params.Flags = 0x6001;
			gs.make_unicode_string(proc_params.ImagePathName, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
			gs.make_unicode_string(proc_params.CommandLine, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
		});

		/*context.ldr.access([&](PEB_LDR_DATA& ldr)
		{
			ldr.InLoadOrderModuleList.Flink = &context.ldr.ptr()->InLoadOrderModuleList;
			ldr.InLoadOrderModuleList.Blink = ldr.InLoadOrderModuleList.Flink;

			ldr.InMemoryOrderModuleList.Flink = &context.ldr.ptr()->InMemoryOrderModuleList;
			ldr.InMemoryOrderModuleList.Blink = ldr.InMemoryOrderModuleList.Flink;

			ldr.InInitializationOrderModuleList.Flink = &context.ldr.ptr()->InInitializationOrderModuleList;
			ldr.InInitializationOrderModuleList.Blink = ldr.InInitializationOrderModuleList.Flink;
		});*/

		return context;
	}

	std::vector<uint8_t> load_file(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::in | std::ios::binary);
		return {(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()};
	}

	mapped_binary map_file(const unicorn& uc, const std::filesystem::path& file)
	{
		const auto data = load_file(file);
		return map_module(uc, data, file.generic_string());
	}

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

	uint32_t get_memory_protection(const unicorn& uc, uint64_t address)
	{
		uint32_t count{};
		uc_mem_region* regions{};

		e(uc_mem_regions(uc, &regions, &count));
		const auto _ = utils::finally([&]
		{
			uc_free(regions);
		});

		for (const auto& region : std::span(regions, count))
		{
			if (is_within_start_and_end(address, region.begin, region.end))
			{
				return region.perms;
			}
		}

		return UC_PROT_NONE;
	}

	uint32_t map_nt_to_unicorn_protection(const uint32_t nt_protection)
	{
		switch (nt_protection)
		{
		case PAGE_NOACCESS:
			return UC_PROT_NONE;
		case PAGE_READONLY:
			return UC_PROT_READ;
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
			return UC_PROT_READ | UC_PROT_WRITE;
		case PAGE_EXECUTE:
		case PAGE_EXECUTE_READ:
			return UC_PROT_READ | UC_PROT_EXEC;
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
		default:
			return UC_PROT_ALL;
		}
	}

	uint32_t map_unicorn_to_nt_protection(const uint32_t unicorn_protection)
	{
		const bool has_exec = unicorn_protection & UC_PROT_EXEC;
		const bool has_read = unicorn_protection & UC_PROT_READ;
		const bool has_write = unicorn_protection & UC_PROT_WRITE;

		if (!has_read)
		{
			return PAGE_NOACCESS;
		}

		if (has_exec && has_write)
		{
			return PAGE_EXECUTE_READWRITE;
		}

		if (has_exec)
		{
			return PAGE_EXECUTE_READ;
		}

		if (has_write)
		{
			return PAGE_READWRITE;
		}

		return PAGE_READONLY;
	}

	void handle_NtManageHotPatch(const unicorn& uc)
	{
		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
	}

	void handle_NtOpenKey(const unicorn& uc)
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

	void handle_NtQuerySystemInformation(const unicorn& uc, const process_context& context)
	{
		const auto info_class = uc.reg<uint32_t>(UC_X86_REG_R10D);
		const auto system_information = uc.reg(UC_X86_REG_RDX);
		const auto system_information_length = uc.reg<uint32_t>(UC_X86_REG_R8D);
		const unicorn_object<uint32_t> return_length{uc, uc.reg(UC_X86_REG_R9)};

		if (info_class == SystemFlushInformation)
		{
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_NOT_SUPPORTED);
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
			uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_BUFFER_OVERFLOW);
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

	void handle_NtQueryProcessInformation(const unicorn& uc, const process_context& context)
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
		e(uc_mem_protect(uc, address, size, requested_protection));

		uc.reg<uint64_t>(UC_X86_REG_RAX, STATUS_SUCCESS);
	}

	void run()
	{
		const unicorn uc{UC_ARCH_X86, UC_MODE_64};

		setup_kusd(uc);
		auto context = setup_teb_and_peb(uc);

		context.executable = map_file(uc, R"(C:\Users\mauri\Desktop\ConsoleApplication6.exe)");

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = reinterpret_cast<void*>(context.executable.image_base);
		});

		context.ntdll = map_file(uc, R"(C:\Windows\System32\ntdll.dll)");

		const auto entry1 = context.ntdll.exports.at("LdrInitializeThunk");
		const auto entry2 = context.ntdll.exports.at("RtlUserThreadStart");

		(void)entry1;
		(void)entry2;

		std::vector<unicorn_hook> export_hooks{};


		std::unordered_map<uint64_t, std::string> export_remap{};
		for (const auto& exp : context.ntdll.exports)
		{
			export_remap.try_emplace(exp.second, exp.first);
		}

		for (const auto& exp : export_remap)
		{
			auto name = exp.second;
			unicorn_hook hook(uc, UC_HOOK_CODE, exp.first, exp.first,
			                  [n = std::move(name)](const unicorn& uc, const uint64_t address, const uint32_t)
			                  {
				                  printf("Executing function: %s (%llX)\n", n.c_str(), address);

				                  if (n == "RtlImageNtHeaderEx")
				                  {
					                  printf("Base: %llX\n", uc.reg(UC_X86_REG_RDX));
				                  }
			                  });

			export_hooks.emplace_back(std::move(hook));
		}

		unicorn_hook hook(uc, UC_HOOK_INSN, 0, std::numeric_limits<uint64_t>::max(),
		                  [&](const unicorn&, const uint64_t address, const uint32_t /*size*/)
		                  {
			                  const auto syscall_id = uc.reg<uint32_t>(UC_X86_REG_EAX);

			                  printf("Handling syscall: %X (%llX)\n", syscall_id, address);

			                  try
			                  {
				                  switch (syscall_id)
				                  {
				                  case 0x12:
					                  handle_NtOpenKey(uc);
					                  break;
				                  case 0x19:
					                  handle_NtQueryProcessInformation(uc, context);
					                  break;
				                  case 0x23:
					                  handle_NtQueryVirtualMemory(uc, context);
					                  break;
				                  case 0x31:
					                  handle_NtQueryPerformanceCounter(uc);
					                  break;
				                  case 0x36:
					                  handle_NtQuerySystemInformation(uc, context);
					                  break;
				                  case 0x48:
					                  handle_NtCreateEvent(uc, context);
					                  break;
				                  case 0x50:
					                  handle_NtProtectVirtualMemory(uc);
					                  break;
				                  case 0x11A:
					                  handle_NtManageHotPatch(uc);
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
		                  }, UC_X86_INS_SYSCALL);

		unicorn_hook hook3(uc, UC_HOOK_MEM_READ, context.peb.value(), context.peb.end(),
		                   [&](const unicorn&, const uint64_t address, const uint32_t /*size*/)
		                   {
			                   printf("Read: %llX - %llX\n", address, address - context.peb.value());
		                   });

		unicorn_hook hook4(uc, UC_HOOK_MEM_READ, context.process_params.value(), context.process_params.end(),
		                   [&](const unicorn&, const uint64_t address, const uint32_t /*size*/)
		                   {
			                   printf("Read2: %llX - %llX\n", address, address - context.process_params.value());
		                   });


		unicorn_hook hook2(uc, UC_HOOK_CODE, 0, std::numeric_limits<uint64_t>::max(),
		                   [](const unicorn& uc, const uint64_t address, const uint32_t /*size*/)
		                   {
			                   /*static bool hit = false;
			                   if (address == 0x01800D46DD)
			                   {
				                   hit = true;
			                   }*/

			                   //if (hit)
			                   {
				                   printf(
					                   "Inst: %16llX - RAX: %16llX - RBX: %16llX - RCX: %16llX - RDX: %16llX - R8: %16llX - R9: %16llX - RDI: %16llX - RSI: %16llX\n",
					                   address,
					                   uc.reg(UC_X86_REG_RAX), uc.reg(UC_X86_REG_RBX), uc.reg(UC_X86_REG_RCX),
					                   uc.reg(UC_X86_REG_RDX), uc.reg(UC_X86_REG_R8), uc.reg(UC_X86_REG_R9),
					                   uc.reg(UC_X86_REG_RDI), uc.reg(UC_X86_REG_RSI));
			                   }
		                   });

		const auto execution_context = context.gs_segment.reserve<CONTEXT>();

		uc.reg(UC_X86_REG_RCX, execution_context.value());
		uc.reg(UC_X86_REG_RDX, context.ntdll.image_base);

		const auto err = uc_emu_start(uc, entry1, 0, 0, 0);
		if (err != UC_ERR_OK)
		{
			uint64_t rip{};
			uc_reg_read(uc, UC_X86_REG_RIP, &rip);
			printf("Emulation failed at: %llX\n", rip);
			e(err);
		}

		printf("Emulation done. Below is the CPU context\n");

		uint64_t rax{};
		e(uc_reg_read(uc, UC_X86_REG_RAX, &rax));

		printf(">>> RAX = 0x%llX\n", rax);
	}
}

int main(int /*argc*/, char** /*argv*/)
{
	try
	{
		run();
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
