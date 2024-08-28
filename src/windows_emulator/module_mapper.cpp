#include "std_include.hpp"
#include "module_mapper.hpp"

namespace
{
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

		printf("Mapping %s at %llX\n", name.c_str(), binary.image_base);

		emu.write_memory(binary.image_base, ptr, optional_header.SizeOfHeaders);

		const std::span sections(IMAGE_FIRST_SECTION(nt_headers), nt_headers->FileHeader.NumberOfSections);

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

		auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (export_directory_entry.VirtualAddress == 0 || export_directory_entry.Size == 0)
		{
			return binary;
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

		return binary;
	}

	std::vector<uint8_t> load_file(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::in | std::ios::binary);
		return {(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()};
	}
}

mapped_binary map_file(x64_emulator& emu, const std::filesystem::path& file)
{
	const auto data = load_file(file);
	return map_module(emu, data, file.generic_string());
}
