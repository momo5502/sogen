#include "std_include.hpp"
#include "module_mapper.hpp"
#include <address_utils.hpp>

namespace
{
	void collect_exports(mapped_binary& binary, const unsigned char* ptr, const IMAGE_OPTIONAL_HEADER& optional_header)
	{
		auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (export_directory_entry.VirtualAddress == 0 || export_directory_entry.Size == 0)
		{
			return;
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
			exported_symbol symbol{};
			symbol.ordinal = ordinals[i];
			symbol.name = reinterpret_cast<const char*>(ptr + names[i]);
			symbol.rva = functions[symbol.ordinal];
			symbol.address = binary.image_base + symbol.rva;

			binary.exports.push_back(std::move(symbol));
		}
	}

	void apply_relocations(x64_emulator& emu, const mapped_binary& binary,
	                       const IMAGE_OPTIONAL_HEADER& optional_header)
	{
		const auto delta = binary.image_base - optional_header.ImageBase;
		if (delta == 0)
		{
			return;
		}

		const auto directory = &optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
		if (directory->Size == 0)
		{
			return;
		}

		std::vector<uint8_t> memory{};
		memory.resize(binary.size_of_image);
		emu.read_memory(binary.image_base, memory.data(), memory.size());

		const auto start = memory.data() + directory->VirtualAddress;
		const auto end = start + directory->Size;

		const auto* relocation = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(start);

		while (reinterpret_cast<const uint8_t*>(relocation) < end)
		{
			if (relocation->VirtualAddress <= 0 || relocation->SizeOfBlock <= 0)
			{
				break;
			}

			const auto dest = memory.data() + relocation->VirtualAddress;

			const auto data_size = relocation->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION);
			const auto entry_count = data_size / sizeof(uint16_t);

			const auto entry_start = offset_pointer<uint16_t>(relocation, sizeof(IMAGE_BASE_RELOCATION));
			const auto entries = std::span(entry_start, entry_count);

			for (const auto entry : entries)
			{
				const int type = entry >> 12;
				const int offset = entry & 0xfff;

				switch (type)
				{
				case IMAGE_REL_BASED_ABSOLUTE:
					break;

				case IMAGE_REL_BASED_HIGHLOW:
					*reinterpret_cast<DWORD*>(dest + offset) += static_cast<DWORD>(delta);
					break;

				case IMAGE_REL_BASED_DIR64:
					*reinterpret_cast<ULONGLONG*>(dest + offset) += delta;
					break;

				default:
					throw std::runtime_error("Unknown relocation type: " + std::to_string(type));
				}
			}

			relocation = offset_pointer<IMAGE_BASE_RELOCATION>(relocation, relocation->SizeOfBlock);
		}

		emu.write_memory(binary.image_base, memory.data(), memory.size());
	}

	void map_sections(x64_emulator& emu, const mapped_binary& binary, const unsigned char* ptr,
	                  const IMAGE_NT_HEADERS& nt_headers)
	{
		const std::span sections(IMAGE_FIRST_SECTION(&nt_headers), nt_headers.FileHeader.NumberOfSections);

		for (const auto& section : sections)
		{
			const auto target_ptr = binary.image_base + section.VirtualAddress;

			if (section.SizeOfRawData > 0)
			{
				const void* source_ptr = ptr + section.PointerToRawData;

				const auto size_of_data = std::min(section.SizeOfRawData, section.Misc.VirtualSize);
				emu.write_memory(target_ptr, source_ptr, size_of_data);
			}

			auto permissions = memory_permission::none;

			if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				permissions |= memory_permission::exec;
			}

			if (section.Characteristics & IMAGE_SCN_MEM_READ)
			{
				permissions |= memory_permission::read;
			}

			if (section.Characteristics & IMAGE_SCN_MEM_WRITE)
			{
				permissions |= memory_permission::write;
			}

			const auto size_of_section = page_align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize));

			emu.protect_memory(target_ptr, size_of_section, permissions, nullptr);
		}
	}

	void hook_exports(emulator& emu, const mapped_binary& binary, const std::filesystem::path& file)
	{
		const auto filename = file.filename().string();

		std::unordered_map<uint64_t, std::string> export_remap{};
		for (const auto& symbol : binary.exports)
		{
			export_remap.try_emplace(symbol.address, symbol.name);
		}

		for (const auto& exp : export_remap)
		{
			auto name = exp.second;
			emu.hook_memory_execution(exp.first, 0,
			                          [n = std::move(name), filename](const uint64_t address, const size_t)
			                          {
				                          printf("Executing function: %s - %s (%llX)\n", filename.c_str(), n.c_str(),
				                                 address);
			                          });
		}
	}

	mapped_binary map_module(x64_emulator& emu, const std::vector<uint8_t>& module_data,
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

		if (!emu.allocate_memory(binary.image_base, binary.size_of_image, memory_permission::read))
		{
			binary.image_base = emu.find_free_allocation_base(binary.size_of_image);
			if ((optional_header.DllCharacteristics &
					IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) == 0 || //
				!emu.allocate_memory(
					binary.image_base, binary.size_of_image, memory_permission::read))
			{
				throw std::runtime_error("Failed to map binary");
			}
		}


		binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;

		printf("Mapping %s at %llX\n", name.c_str(), binary.image_base);

		emu.write_memory(binary.image_base, ptr, optional_header.SizeOfHeaders);

		map_sections(emu, binary, ptr, *nt_headers);
		apply_relocations(emu, binary, optional_header);

		static int i = 0;
		if (++i < 3)
		{
			collect_exports(binary, ptr, optional_header);
		}

		return binary;
	}

	std::vector<uint8_t> load_file(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::in | std::ios::binary);
		return {(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()};
	}
}

std::optional<mapped_binary> map_file(x64_emulator& emu, const std::filesystem::path& file)
{
	const auto data = load_file(file);
	if (data.empty())
	{
		return {};
	}

	auto binary = map_module(emu, data, file.generic_string());

	hook_exports(emu, binary, file);

	return binary;
}
