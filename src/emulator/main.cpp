#include "std_include.hpp"

#include "unicorn.hpp"
#include "memory_utils.hpp"
#include "unicorn_utils.hpp"
#include "process_context.hpp"
#include "syscalls.hpp"

#define GS_SEGMENT_ADDR 0x6000000ULL
#define GS_SEGMENT_SIZE (20 << 20)  // 20 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_SIZE 0x40000
#define STACK_ADDRESS (0x800000000000 - STACK_SIZE)

#define KUSD_ADDRESS 0x7ffe0000

namespace
{
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
		                  [&](const unicorn&, const uint64_t, const uint32_t)
		                  {
			                  handle_syscall(uc, context);
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
