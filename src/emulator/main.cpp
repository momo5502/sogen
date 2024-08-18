#include "std_include.hpp"

#define GS_SEGMENT_ADDR 0x6000000ULL
#define GS_SEGMENT_SIZE (20 << 20)  // 20 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_ADDRESS 0x7ffffffde000
#define STACK_SIZE 0x40000

#define KUSD_ADDRESS 0x7ffe0000

#include "unicorn.hpp"

namespace
{
	uint64_t align_down(const uint64_t value, const uint64_t alignment)
	{
		return value & ~(alignment - 1);
	}

	uint64_t align_up(const uint64_t value, const uint64_t alignment)
	{
		return align_down(value + (alignment - 1), alignment);
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

		T* ptr() const
		{
			return reinterpret_cast<T*>(this->address_);
		}

		template <typename F>
		void access(const F& accessor) const
		{
			T obj{};

			e(uc_mem_read(*this->uc_, this->address_, &obj, sizeof(obj)));

			accessor(obj);

			e(uc_mem_write(*this->uc_, this->address_, &obj, sizeof(obj)));
		}

	private:
		const unicorn* uc_{};
		uint64_t address_{};
	};

	class unicorn_allocator
	{
	public:
		unicorn_allocator(const unicorn& uc, const uint64_t address, const uint64_t size)
			: uc_(&uc)
			  , address_(address)
			  , size_(size)
			  , active_address_(address)
		{
		}

		template <typename T>
		unicorn_object<T> reserve()
		{
			const auto alignment = alignof(T);
			const auto potential_start = align_up(this->active_address_, alignment);
			const auto potential_end = potential_start + sizeof(T);
			const auto total_end = this->address_ + this->size_;

			if (potential_end > total_end)
			{
				throw std::runtime_error("Out of memory");
			}

			this->active_address_ = potential_end;

			return unicorn_object<T>(*this->uc_, potential_start);
		}

	private:
		const unicorn* uc_{};
		const uint64_t address_{};
		const uint64_t size_{};
		uint64_t active_address_{0};
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
		/* TODO: Fix
		uc_mem_map(uc, KUSD_ADDRESS, sizeof(KUSER_SHARED_DATA), UC_PROT_READ);

		unicorn_object<KUSER_SHARED_DATA> kusd_object{uc, KUSD_ADDRESS};
		*/

		uc_mem_map_ptr(uc, KUSD_ADDRESS, sizeof(KUSER_SHARED_DATA), UC_PROT_READ,
		               reinterpret_cast<void*>(KUSD_ADDRESS));
	}

	std::unordered_map<std::string, uint64_t> map_module(const unicorn& uc, const std::vector<uint8_t>& module_data)
	{
		// TODO: Range checks
		auto* ptr = module_data.data();
		auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(ptr);
		auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(ptr + dos_header->e_lfanew);
		auto& optional_header = nt_headers->OptionalHeader;

		auto prefered_base = optional_header.ImageBase;

		while (true)
		{
			const auto res = uc_mem_map(uc, prefered_base, optional_header.SizeOfImage, UC_PROT_READ);
			if (res == UC_ERR_OK)
			{
				break;
			}

			prefered_base += 0x10000;

			if (prefered_base < optional_header.ImageBase || (optional_header.DllCharacteristics &
				IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) == 0)
			{
				throw std::runtime_error("Failed to map range");
			}
		}

		e(uc_mem_write(uc, prefered_base, ptr, optional_header.SizeOfHeaders));

		const std::span sections(IMAGE_FIRST_SECTION(nt_headers), nt_headers->FileHeader.NumberOfSections);

		for (const auto& section : sections)
		{
			const auto target_ptr = prefered_base + section.VirtualAddress;

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

			const auto size_of_section = align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize), 0x1000);

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

		std::unordered_map<std::string, uint64_t> exports{};

		for (DWORD i = 0; i < names_count; i++)
		{
			const auto* function_name = reinterpret_cast<const char*>(ptr + names[i]);
			const auto function_rva = functions[ordinals[i]];
			const auto function_address = prefered_base + function_rva;

			exports[function_name] = function_address;
		}

		return exports;
	}

	void setup_teb_and_peb(const unicorn& uc)
	{
		setup_stack(uc, STACK_ADDRESS, STACK_SIZE);
		auto gs = setup_gs_segment(uc, GS_SEGMENT_ADDR, GS_SEGMENT_SIZE);

		const auto teb_object = gs.reserve<TEB>();
		const auto peb_object = gs.reserve<PEB>();
		const auto ldr_object = gs.reserve<PEB_LDR_DATA>();

		teb_object.access([&](TEB& teb)
		{
			teb.NtTib.StackLimit = reinterpret_cast<void*>(STACK_ADDRESS);
			teb.NtTib.StackBase = reinterpret_cast<void*>((STACK_ADDRESS + STACK_SIZE));
			teb.NtTib.Self = &teb_object.ptr()->NtTib;
			teb.ProcessEnvironmentBlock = peb_object.ptr();
		});

		peb_object.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = nullptr;
			peb.Ldr = ldr_object.ptr();
		});

		ldr_object.access([&](PEB_LDR_DATA& ldr)
		{
			ldr.InLoadOrderModuleList.Flink = &ldr_object.ptr()->InLoadOrderModuleList;
			ldr.InLoadOrderModuleList.Blink = ldr.InLoadOrderModuleList.Flink;

			ldr.InMemoryOrderModuleList.Flink = &ldr_object.ptr()->InMemoryOrderModuleList;
			ldr.InMemoryOrderModuleList.Blink = ldr.InMemoryOrderModuleList.Flink;

			ldr.InInitializationOrderModuleList.Flink = &ldr_object.ptr()->InInitializationOrderModuleList;
			ldr.InInitializationOrderModuleList.Blink = ldr.InInitializationOrderModuleList.Flink;
		});
	}

	std::vector<uint8_t> load_file(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::in | std::ios::binary);
		return {(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()};
	}

	std::unordered_map<std::string, uint64_t> map_file(const unicorn& uc, const std::filesystem::path& file)
	{
		const auto data = load_file(file);
		return map_module(uc, data);
	}

	void run()
	{
		const unicorn uc{UC_ARCH_X86, UC_MODE_64};

		setup_kusd(uc);
		setup_teb_and_peb(uc);

		const auto executable_exports = map_file(uc, R"(C:\Users\mauri\Desktop\ConsoleApplication6.exe)");

		const auto ntdll_exports = map_file(uc, R"(C:\Windows\System32\ntdll.dll)");

		const auto entry1 = ntdll_exports.at("LdrInitializeThunk");
		const auto entry2 = ntdll_exports.at("RtlUserThreadStart");

		//e(uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(X86_CODE32) - 1, 0, 0));

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
		MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
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
