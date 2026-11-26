#include "../std_include.hpp"
#include "module_mapping.hpp"
#include <address_utils.hpp>
#include <algorithm>
#include <bit>
#include <limits>
#include <new>

#include <utils/io.hpp>
#include <utils/buffer_accessor.hpp>
#include <utils/string.hpp>
#include <platform/win_pefile_debug.hpp>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

namespace sogen
{

    namespace
    {
        memory_permission get_section_permissions(const IMAGE_SECTION_HEADER& section, const PEMachineType machine)
        {
            auto permissions = memory_permission::none;

            // ARM64EC images can mark the hybrid x64 entry/bootstrap
            // range as CNT_CODE without setting MEM_EXECUTE.  Windows
            // applies the CHPE code map when it creates the image VAD;
            // Sogen's x64 backend needs the same treatment for file-backed
            // mappings, otherwise a hybrid image entry remains
            // non-executable and is never reached.
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                (machine == PEMachineType::ARM64EC && (section.Characteristics & IMAGE_SCN_CNT_CODE)))
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

            return permissions;
        }

        template <typename T>
        void validate_image_machine(const PENTHeaders_t<T>& headers)
        {
            const auto machine = headers.FileHeader.Machine;
            const bool supported = sizeof(T) == sizeof(uint32_t) ? machine == PEMachineType::I386
                                                                 : machine == PEMachineType::AMD64 || machine == PEMachineType::ARM64 ||
                                                                       machine == PEMachineType::ARM64EC;
            if (!supported || headers.Signature != PENTHeaders_t<T>::k_Signature ||
                headers.OptionalHeader.Magic != PEOptionalHeader_t<T>::k_Magic)
            {
                throw std::runtime_error("Unsupported or inconsistent PE machine and optional header");
            }
        }

        template <typename T>
        PENTHeaders_t<T> read_nt_headers(const utils::safe_buffer_accessor<const std::byte>& buffer, const size_t nt_headers_offset)
        {
            PENTHeaders_t<T> nt_headers{};
            constexpr auto optional_offset = offsetof(PENTHeaders_t<T>, OptionalHeader);
            std::memcpy(&nt_headers, buffer.get_pointer_for_range(nt_headers_offset, optional_offset), optional_offset);
            constexpr auto directories_offset = offsetof(PEOptionalHeader_t<T>, DataDirectory);
            const auto optional_size = nt_headers.FileHeader.SizeOfOptionalHeader;
            if (optional_size < directories_offset)
            {
                throw std::runtime_error("Optional header is too small");
            }
            const auto* optional_data = buffer.get_pointer_for_range(nt_headers_offset + optional_offset, optional_size);
            std::memcpy(&nt_headers.OptionalHeader, optional_data, std::min<size_t>(optional_size, sizeof(nt_headers.OptionalHeader)));
            auto& directories = nt_headers.OptionalHeader.DataDirectory;
            const auto directory_count = std::min<size_t>({nt_headers.OptionalHeader.NumberOfRvaAndSizes, std::size(directories),
                                                           (optional_size - directories_offset) / sizeof(directories[0])});
            std::fill(std::begin(directories) + directory_count, std::end(directories), PEDirectory_t2{});
            nt_headers.OptionalHeader.NumberOfRvaAndSizes = static_cast<uint32_t>(directory_count);
            return nt_headers;
        }

        bool must_map_module_below_4gb(const std::string& module_name, const PEMachineType machine)
        {
            if (machine != PEMachineType::AMD64)
            {
                return false;
            }

            // wow64 startup needs wow64cpu.dll to be reachable through 32-bit address
            if (!utils::string::equals_ignore_case(std::string_view{module_name}, std::string_view{"wow64cpu.dll"}))
            {
                return false;
            }

            return true;
        }

        template <typename T>
        void collect_pe_sections(mapped_module& binary, const utils::safe_buffer_accessor<const std::byte>& buffer,
                                 const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset)
        {
            binary.pe_sections.clear();

            const auto section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
            const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(section_offset));
            binary.pe_sections.reserve(nt_headers.FileHeader.NumberOfSections);

            for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
            {
                const auto section = sections.get(i);
                binary.pe_sections.emplace_back(pe_section_symbol_mapping{
                    .virtual_address = section.VirtualAddress,
                    .virtual_size = section.Misc.VirtualSize,
                    .raw_size = section.SizeOfRawData,
                });
            }
        }

        template <typename T>
        void collect_pdb_signature(mapped_module& binary, const utils::safe_buffer_accessor<const std::byte>& buffer,
                                   const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset, const bool mapped_image)
        {
            try
            {
                const auto layout = mapped_image ? winpe::image_layout::mapped : winpe::image_layout::file;
                binary.pdb = winpe::read_pdb_signature(buffer, nt_headers, nt_headers_offset, layout);
            }
            catch (...)
            {
                binary.pdb.reset();
            }
        }

        template <typename T>
        std::vector<std::byte> read_mapped_memory(const memory_manager& memory, const mapped_module& binary)
        {
            std::vector<std::byte> mem{};
            mem.resize(static_cast<size_t>(binary.size_of_image));
            memory.read_memory(binary.image_base, mem.data(), mem.size());

            return mem;
        }

        bool image_range_fits(const uint64_t image_size, const uint64_t rva, const uint64_t count, const uint64_t stride)
        {
            return count == 0 || (rva < image_size && count <= (image_size - rva) / stride);
        }

        template <typename T>
        void collect_exports(mapped_module& binary, const utils::safe_buffer_accessor<const std::byte> buffer,
                             const PEOptionalHeader_t<T>& optional_header)
        {
            const auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (export_directory_entry.VirtualAddress == 0 || export_directory_entry.Size == 0)
            {
                return;
            }

            const auto export_directory = buffer.as<IMAGE_EXPORT_DIRECTORY>(export_directory_entry.VirtualAddress).get();

            const auto names_count = export_directory.NumberOfNames;
            const auto function_count = export_directory.NumberOfFunctions;

            // An out-of-bounds array is not always an out-of-bounds read: a truncated one whose first
            // entries still land inside the image yields exports invented from unrelated bytes.
            const uint64_t image_size = buffer.get_buffer().size();
            if (!image_range_fits(image_size, export_directory.AddressOfNames, names_count, sizeof(DWORD)) ||
                !image_range_fits(image_size, export_directory.AddressOfNameOrdinals, names_count, sizeof(WORD)) ||
                !image_range_fits(image_size, export_directory.AddressOfFunctions, function_count, sizeof(DWORD)))
            {
                return;
            }

            const auto names = buffer.as<DWORD>(export_directory.AddressOfNames);
            const auto ordinals = buffer.as<WORD>(export_directory.AddressOfNameOrdinals);
            const auto functions = buffer.as<DWORD>(export_directory.AddressOfFunctions);

            binary.exports.reserve(function_count);

            std::unordered_map<WORD, DWORD> name_index_by_ordinal{};
            name_index_by_ordinal.reserve(names_count);
            for (DWORD i = 0; i < names_count; i++)
            {
                const auto ordinal = ordinals.get(i);
                if (ordinal >= function_count)
                {
                    return;
                }

                name_index_by_ordinal[ordinal] = i;
            }

            // AddressOfNames covers only part of AddressOfFunctions: exports without a name-table entry
            // are still real exports and need a symbol.
            for (DWORD ordinal = 0; ordinal < function_count; ordinal++)
            {
                const auto rva = functions.get(ordinal);
                if (rva == 0)
                {
                    continue; // Unused ordinal slot.
                }

                exported_symbol symbol{};
                symbol.ordinal = export_directory.Base + ordinal;
                symbol.rva = rva;
                symbol.address = binary.image_base + symbol.rva;

                const auto name_index = name_index_by_ordinal.find(static_cast<WORD>(ordinal));
                if (name_index != name_index_by_ordinal.end())
                {
                    symbol.name = buffer.as_string(names.get(name_index->second));
                }
                else
                {
                    symbol.name = "#" + std::to_string(symbol.ordinal);
                }

                binary.exports.push_back(std::move(symbol));
            }

            for (const auto& symbol : binary.exports)
            {
                binary.address_names.try_emplace(symbol.address, symbol.name);
            }
        }

        template <typename T>
            requires(std::is_integral_v<T>)
        void apply_relocation(const utils::safe_buffer_accessor<std::byte> block, const uint64_t offset, const uint64_t delta)
        {
            auto* const pointer = block.get_pointer_for_range(static_cast<size_t>(offset), sizeof(T));

            T value{};
            std::memcpy(&value, pointer, sizeof(value));
            value += static_cast<T>(delta);
            std::memcpy(pointer, &value, sizeof(value));
        }

        template <typename T>
        T read_mapped_object(const memory_manager& memory, const uint64_t address)
        {
            T value{};
            memory.read_memory(address, &value, sizeof(value));
            return value;
        }

        std::string read_mapped_string(const memory_manager& memory, const uint64_t address)
        {
            std::string value{};

            for (size_t i = 0;; ++i)
            {
                char c{};
                memory.read_memory(address + i, &c, sizeof(c));
                if (c == '\0')
                {
                    break;
                }

                value.push_back(c);
            }

            return value;
        }

        template <typename T>
        void collect_imports(mapped_module& binary, const memory_manager& memory, const PEOptionalHeader_t<T>& optional_header)
        {
            const auto& import_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (import_directory_entry.VirtualAddress == 0 || import_directory_entry.Size == 0)
            {
                return;
            }

            const auto import_base = binary.image_base + import_directory_entry.VirtualAddress;

            for (size_t i = 0;; ++i)
            {
                const auto descriptor =
                    read_mapped_object<IMAGE_IMPORT_DESCRIPTOR>(memory, import_base + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
                if (!descriptor.Name)
                {
                    break;
                }

                using thunk_traits = thunk_data_traits<T>;
                using thunk_type = thunk_traits::type;

                const auto module_index = binary.imported_modules.size();
                binary.imported_modules.push_back(read_mapped_string(memory, binary.image_base + descriptor.Name));

                auto original_thunk_rva = descriptor.FirstThunk;
                if (descriptor.OriginalFirstThunk)
                {
                    original_thunk_rva = descriptor.OriginalFirstThunk;
                }

                for (size_t j = 0;; ++j)
                {
                    const auto original_thunk =
                        read_mapped_object<thunk_type>(memory, binary.image_base + original_thunk_rva + sizeof(thunk_type) * j);
                    if (!original_thunk.u1.AddressOfData)
                    {
                        break;
                    }

                    static_assert(sizeof(thunk_type) == sizeof(T));
                    const auto thunk_rva = descriptor.FirstThunk + sizeof(thunk_type) * j;
                    const auto thunk_address = thunk_rva + binary.image_base;

                    auto& sym = binary.imports[thunk_address];
                    sym.module_index = module_index;

                    if (thunk_traits::snap_by_ordinal(original_thunk.u1.Ordinal))
                    {
                        sym.name = "#" + std::to_string(thunk_traits::ordinal_mask(original_thunk.u1.Ordinal));
                    }
                    else
                    {
                        // BUG: ntdll ldr discards highpart. this handles intentionally malformed PEs.
                        const auto rva_of_data = static_cast<uint32_t>(original_thunk.u1.AddressOfData);
                        const auto by_name_address = binary.image_base + rva_of_data + offsetof(IMAGE_IMPORT_BY_NAME, Name);
                        sym.name = read_mapped_string(memory, by_name_address);
                    }
                }
            }
        }

        template <typename T>
        void collect_exports(mapped_module& binary, const memory_manager& memory, const PEOptionalHeader_t<T>& optional_header)
        {
            const auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (export_directory_entry.VirtualAddress == 0 || export_directory_entry.Size == 0)
            {
                return;
            }

            const auto export_directory =
                read_mapped_object<IMAGE_EXPORT_DIRECTORY>(memory, binary.image_base + export_directory_entry.VirtualAddress);

            const auto names_count = export_directory.NumberOfNames;
            const auto function_count = export_directory.NumberOfFunctions;

            const auto image_size = binary.size_of_image;
            if (!image_range_fits(image_size, export_directory.AddressOfNames, names_count, sizeof(DWORD)) ||
                !image_range_fits(image_size, export_directory.AddressOfNameOrdinals, names_count, sizeof(WORD)) ||
                !image_range_fits(image_size, export_directory.AddressOfFunctions, function_count, sizeof(DWORD)))
            {
                return;
            }

            binary.exports.reserve(function_count);

            std::unordered_map<WORD, DWORD> name_index_by_ordinal{};
            name_index_by_ordinal.reserve(names_count);
            for (DWORD i = 0; i < names_count; i++)
            {
                const auto ordinal =
                    read_mapped_object<WORD>(memory, binary.image_base + export_directory.AddressOfNameOrdinals + i * sizeof(WORD));
                if (ordinal >= function_count)
                {
                    return;
                }

                name_index_by_ordinal[ordinal] = i;
            }

            for (DWORD ordinal = 0; ordinal < function_count; ordinal++)
            {
                const auto function_rva =
                    read_mapped_object<DWORD>(memory, binary.image_base + export_directory.AddressOfFunctions + ordinal * sizeof(DWORD));
                if (function_rva == 0)
                {
                    continue; // Unused ordinal slot.
                }

                exported_symbol symbol{};
                symbol.ordinal = export_directory.Base + ordinal;
                symbol.rva = function_rva;
                symbol.address = binary.image_base + symbol.rva;

                const auto name_index = name_index_by_ordinal.find(static_cast<WORD>(ordinal));
                if (name_index != name_index_by_ordinal.end())
                {
                    const auto name_rva = read_mapped_object<DWORD>(memory, binary.image_base + export_directory.AddressOfNames +
                                                                                name_index->second * sizeof(DWORD));
                    symbol.name = read_mapped_string(memory, binary.image_base + name_rva);
                }
                else
                {
                    symbol.name = "#" + std::to_string(symbol.ordinal);
                }

                binary.exports.push_back(std::move(symbol));
            }

            for (const auto& symbol : binary.exports)
            {
                binary.address_names.try_emplace(symbol.address, symbol.name);
            }
        }

        // A process launch never parses the image's own export table, so Windows starts an EXE whose export
        // data-directory points at junk. Reading one off the end of the image must not fail the whole map.
        template <typename T, typename Source>
        void collect_exports_or_ignore(mapped_module& binary, const Source& source, const PEOptionalHeader_t<T>& optional_header)
        {
            try
            {
                collect_exports(binary, source, optional_header);
            }
            catch (...)
            {
                binary.exports.clear();
                binary.address_names.clear();
            }
        }

        // Fixups are applied to a host-side mirror of the block's guest byte span and committed with a
        // single write_memory. Every guest write triggers a JIT translation invalidation, so doing this
        // per fixup costs thousands of invalidations per module instead of one per block.
        void apply_relocation_block(memory_manager& memory, const uint64_t image_base, const IMAGE_BASE_RELOCATION& relocation,
                                    const std::span<const uint16_t> entries, const uint64_t delta)
        {
            uint64_t min_offset = 0;
            uint64_t max_end = 0;
            bool has_fixup = false;

            for (size_t index = 0; index < entries.size(); ++index)
            {
                const auto entry = entries[index];
                const int type = entry >> 12;
                const auto offset = static_cast<uint16_t>(entry & 0xfff);
                const auto total_offset = static_cast<uint64_t>(relocation.VirtualAddress) + offset;

                size_t width = 0;
                switch (type)
                {
                case IMAGE_REL_BASED_ABSOLUTE:
                    continue;
                case IMAGE_REL_BASED_HIGHADJ:
                    if (++index >= entries.size())
                    {
                        throw std::runtime_error("Missing HIGHADJ relocation addend");
                    }
                    [[fallthrough]];
                case IMAGE_REL_BASED_HIGH:
                case IMAGE_REL_BASED_LOW:
                    width = sizeof(WORD);
                    break;
                case IMAGE_REL_BASED_HIGHLOW:
                    width = sizeof(DWORD);
                    break;
                case IMAGE_REL_BASED_DIR64:
                    width = sizeof(ULONGLONG);
                    break;
                default:
                    throw std::runtime_error("Unknown relocation type: " + std::to_string(type));
                }

                if (!has_fixup)
                {
                    min_offset = total_offset;
                    max_end = total_offset + width;
                    has_fixup = true;
                }
                else
                {
                    min_offset = std::min(min_offset, total_offset);
                    max_end = std::max(max_end, total_offset + width);
                }
            }

            if (!has_fixup)
            {
                return;
            }

            const auto span_size = static_cast<size_t>(max_end - min_offset);
            auto block_buffer = memory.read_memory(image_base + min_offset, span_size);

            const utils::safe_buffer_accessor<std::byte> block{block_buffer};

            for (size_t index = 0; index < entries.size(); ++index)
            {
                const auto entry = entries[index];
                const int type = entry >> 12;
                const auto offset = static_cast<uint16_t>(entry & 0xfff);
                const auto total_offset = static_cast<uint64_t>(relocation.VirtualAddress) + offset;
                const auto local_offset = static_cast<size_t>(total_offset - min_offset);

                switch (type)
                {
                case IMAGE_REL_BASED_ABSOLUTE:
                    break;

                case IMAGE_REL_BASED_HIGH:
                    apply_relocation<WORD>(block, local_offset, delta >> 16);
                    break;

                case IMAGE_REL_BASED_LOW:
                    apply_relocation<WORD>(block, local_offset, delta);
                    break;

                case IMAGE_REL_BASED_HIGHADJ: {
                    const auto high = block.as<WORD>(local_offset);
                    const auto low = static_cast<int16_t>(entries[++index]);
                    const uint32_t adjusted =
                        (static_cast<uint32_t>(high.get()) << 16) + static_cast<uint32_t>(low) + static_cast<uint32_t>(delta) + 0x8000u;
                    high.set(static_cast<WORD>(adjusted >> 16));
                    break;
                }

                case IMAGE_REL_BASED_HIGHLOW:
                    apply_relocation<DWORD>(block, local_offset, delta);
                    break;

                case IMAGE_REL_BASED_DIR64:
                    apply_relocation<ULONGLONG>(block, local_offset, delta);
                    break;

                default:
                    throw std::runtime_error("Unknown relocation type: " + std::to_string(type));
                }
            }

            memory.write_memory(image_base + min_offset, block_buffer.data(), span_size);
        }

        template <typename T>
        void apply_relocations(const mapped_module& binary, memory_manager& memory, const PEOptionalHeader_t<T>& optional_header,
                               const uint64_t relocation_base)
        {
            const auto delta = relocation_base - optional_header.ImageBase;
            if (delta == 0)
            {
                return;
            }

            const auto* directory = &optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (directory->Size == 0)
            {
                return;
            }

            auto relocation_offset = directory->VirtualAddress;
            const auto relocation_end = relocation_offset + directory->Size;

            std::vector<uint16_t> entries{};

            while (relocation_offset < relocation_end)
            {
                const auto relocation = read_mapped_object<IMAGE_BASE_RELOCATION>(memory, binary.image_base + relocation_offset);

                if (relocation.VirtualAddress <= 0 || relocation.SizeOfBlock <= sizeof(IMAGE_BASE_RELOCATION) ||
                    relocation.SizeOfBlock > relocation_end - relocation_offset)
                {
                    break;
                }

                const auto data_size = relocation.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION);
                const auto entry_count = static_cast<size_t>(data_size / sizeof(uint16_t));
                const auto entries_base = binary.image_base + relocation_offset + sizeof(IMAGE_BASE_RELOCATION);

                relocation_offset += relocation.SizeOfBlock;

                entries.resize(entry_count);
                if (entry_count > 0)
                {
                    memory.read_memory(entries_base, entries.data(), entry_count * sizeof(uint16_t));
                }

                apply_relocation_block(memory, binary.image_base, relocation, entries, delta);
            }
        }

        // Windows maps an image whose sections are aligned below the page size as a single EXECUTE_WRITECOPY
        // region, ignoring the section characteristics.
        template <typename T>
        bool is_mapped_as_one_region(const PEOptionalHeader_t<T>& optional_header)
        {
            return optional_header.SectionAlignment < page_align_up(1);
        }

        // Windows rejects an image whose sections do not follow each other without gaps, in order, each slot
        // sized by its VirtualSize (SizeOfRawData when that is 0) and rounded up to SectionAlignment. Only a
        // sub-page-aligned image, which Windows maps as the file itself, may have gaps and unaligned sections.
        template <typename T>
        void validate_image_layout(const utils::safe_buffer_accessor<const std::byte>& buffer, const PENTHeaders_t<T>& nt_headers,
                                   const uint64_t nt_headers_offset, const uint64_t file_size)
        {
            const auto& optional_header = nt_headers.OptionalHeader;
            const uint64_t section_alignment = optional_header.SectionAlignment;
            const uint64_t file_alignment = optional_header.FileAlignment;
            const uint64_t image_size = page_align_up(optional_header.SizeOfImage);

            const auto require = [](const bool valid, const NTSTATUS status = STATUS_INVALID_IMAGE_FORMAT) {
                if (!valid)
                {
                    throw invalid_image_format("Invalid image layout", status);
                }
            };

            // The kernel fails an image whose raw data end, rounded up to FileAlignment, wraps 32 bits in any section (without
            // the rounding in a sub-page image), compares only the raw data end of the last section header with the file size,
            // whatever its pointer, and rejects any other section that has raw data at pointer 0 as not starting with MZ.
            const auto raw_data_end_fits = [](const IMAGE_SECTION_HEADER& section, const uint64_t alignment) {
                return section.SizeOfRawData == 0 || align_up(static_cast<uint64_t>(section.PointerToRawData) + section.SizeOfRawData,
                                                              alignment) <= std::numeric_limits<uint32_t>::max();
            };

            require(image_size != 0 && optional_header.SizeOfHeaders <= image_size);
            require(section_alignment != 0 && std::has_single_bit(file_alignment) && file_alignment <= section_alignment);

            const auto first_section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
            const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(first_section_offset));
            const auto section_count = nt_headers.FileHeader.NumberOfSections;

            if (is_mapped_as_one_region(optional_header))
            {
                for (size_t i = 0; i < section_count; ++i)
                {
                    const auto section = sections.get(i);
                    require(raw_data_end_fits(section, 1));
                    require(section.PointerToRawData == section.VirtualAddress && section.Misc.VirtualSize <= section.SizeOfRawData);
                }
                return;
            }

            require(std::has_single_bit(section_alignment) && file_alignment >= 0x200);
            require(section_count != 0);

            uint64_t next_virtual_address = align_up(optional_header.SizeOfHeaders, section_alignment);
            for (size_t i = 0; i < section_count; ++i)
            {
                const auto section = sections.get(i);
                const uint64_t slot_size = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
                require(section.VirtualAddress == next_virtual_address && slot_size != 0);
                require(raw_data_end_fits(section, file_alignment));
                require(i + 1 == section_count || section.PointerToRawData != 0 || section.SizeOfRawData == 0, STATUS_INVALID_IMAGE_NOT_MZ);
                next_virtual_address += align_up(slot_size, section_alignment);
            }

            require(next_virtual_address <= image_size);

            const auto last_section = sections.get(section_count - 1);
            require(last_section.SizeOfRawData == 0 ||
                    static_cast<uint64_t>(last_section.PointerToRawData) + last_section.SizeOfRawData <= file_size);
        }

        constexpr uint64_t sector_size = 0x200;
        constexpr size_t image_header_page_size = 0x1000;
        constexpr size_t image_header_later_pages_size = 2 * image_header_page_size;
        constexpr uint32_t largest_image_size = 0x77000000;
        constexpr uint16_t optional_header_size_alignment = 8;
        constexpr size_t dos_paragraph_size = 16;

        constexpr uint8_t ne_target_unknown = 0;
        constexpr uint8_t ne_target_os2 = 1;
        constexpr uint8_t ne_target_windows = 2;
        constexpr uint8_t ne_target_boss = 5;
        constexpr std::array<uint8_t, 2> ne_windows_16_major_versions{2, 3};

        constexpr std::string_view os2_api_module = "DOSCALLS";
        constexpr size_t os2_bind_stub_offset = 0x200;
        constexpr std::string_view os2_bind_stub = "16STUB";
        constexpr size_t phar_lap_copyright_offset = 24;
        constexpr std::string_view phar_lap_copyright{"Phar Lap Software, Inc.", sizeof("Phar Lap Software, Inc.")};
        constexpr size_t phar_lap_signature_offset = 56;
        constexpr std::array<std::string_view, 3> phar_lap_signatures{"PK", "PO", "PV"};
        constexpr size_t rational_copyright_pointer_offset = 48;
        constexpr std::string_view rational_copyright = "Copyright (C) Rational Systems, Inc.";
        constexpr std::string_view lotus_preloader = "1-2-3 Preloader";

        template <typename T>
        T read_at(const std::span<const std::byte> buffer, const size_t offset)
        {
            T value{};
            if (offset <= buffer.size() && sizeof(T) <= buffer.size() - offset)
            {
                std::memcpy(&value, buffer.data() + offset, sizeof(T));
            }

            return value;
        }

        bool holds_at(const std::span<const std::byte> buffer, const size_t offset, const std::string_view text)
        {
            return offset <= buffer.size() && text.size() <= buffer.size() - offset &&
                   std::memcmp(buffer.data() + offset, text.data(), text.size()) == 0;
        }

        // MiCheckDosCalls
        bool imports_os2_api(const std::span<const std::byte> new_header, const PEOs2Header_t& os2_header)
        {
            const size_t module_table = os2_header.ne_modtab;
            if (os2_header.ne_cmod == 0 || module_table + sizeof(uint16_t) * os2_header.ne_cmod > new_header.size())
            {
                return false;
            }

            for (size_t i = 0; i < os2_header.ne_cmod; ++i)
            {
                const size_t name = os2_header.ne_imptab + size_t{read_at<uint16_t>(new_header, module_table + i * sizeof(uint16_t))};
                if (name >= new_header.size())
                {
                    return false;
                }

                const size_t length = read_at<uint8_t>(new_header, name);
                if (length == 0 || name + length >= new_header.size())
                {
                    return false;
                }

                if (length == os2_api_module.size() && holds_at(new_header, name + 1, os2_api_module))
                {
                    return true;
                }
            }

            return false;
        }

        bool is_phar_lap_extended(const std::span<const std::byte> first_page, const size_t dos_code)
        {
            return dos_code + phar_lap_signature_offset + sizeof(uint16_t) <= first_page.size() &&
                   holds_at(first_page, dos_code + phar_lap_copyright_offset, phar_lap_copyright) &&
                   std::ranges::any_of(phar_lap_signatures, [&](const std::string_view signature) {
                       return holds_at(first_page, dos_code + phar_lap_signature_offset, signature);
                   });
        }

        bool is_rational_extended(const std::span<const std::byte> first_page, const size_t dos_code)
        {
            if (dos_code + rational_copyright_pointer_offset + sizeof(uint16_t) > first_page.size())
            {
                return false;
            }

            const auto copyright = dos_code + read_at<uint16_t>(first_page, dos_code + rational_copyright_pointer_offset);
            return holds_at(first_page, copyright, rational_copyright);
        }

        bool is_lotus_preloaded(const std::span<const std::byte> new_header, const PEOs2Header_t& os2_header,
                                const uint32_t new_header_offset)
        {
            const auto non_resident_names = static_cast<uint32_t>(os2_header.ne_nrestab);
            if (non_resident_names <= new_header_offset)
            {
                return false;
            }

            const size_t first_name = non_resident_names - new_header_offset;
            return first_name + 1 + lotus_preloader.size() < new_header.size() && holds_at(new_header, first_name + 1, lotus_preloader);
        }

        // MiComputeBadImageHeaderType
        NTSTATUS bad_image_header_status(const std::span<const std::byte> first_page, const std::span<const std::byte> new_header)
        {
            const auto os2_header = read_at<PEOs2Header_t>(new_header, 0);
            if (os2_header.ne_magic != PEOs2Header_t::k_Magic)
            {
                return STATUS_INVALID_IMAGE_PROTECT;
            }

            const auto expected_major_version = static_cast<uint8_t>(os2_header.ne_expver >> std::numeric_limits<uint8_t>::digits);
            const auto expects_windows_16 =
                std::ranges::find(ne_windows_16_major_versions, expected_major_version) != ne_windows_16_major_versions.end();
            const auto targets_windows =
                os2_header.ne_exetyp == ne_target_windows || (os2_header.ne_exetyp == ne_target_unknown && expects_windows_16);
            if (targets_windows && !imports_os2_api(new_header, os2_header))
            {
                return STATUS_INVALID_IMAGE_WIN_16;
            }

            if (os2_header.ne_exetyp == ne_target_boss || os2_header.ne_enttab == os2_header.ne_imptab)
            {
                return STATUS_INVALID_IMAGE_PROTECT;
            }

            if (os2_header.ne_exetyp == ne_target_os2 && holds_at(first_page, os2_bind_stub_offset, os2_bind_stub))
            {
                return STATUS_INVALID_IMAGE_PROTECT;
            }

            const auto dos_header = read_at<PEDosHeader_t>(first_page, 0);
            const auto dos_code = size_t{dos_header.e_cparhdr} * dos_paragraph_size;
            if (is_phar_lap_extended(first_page, dos_code) || is_rational_extended(first_page, dos_code) ||
                is_lotus_preloaded(new_header, os2_header, dos_header.e_lfanew))
            {
                return STATUS_INVALID_IMAGE_PROTECT;
            }

            return STATUS_INVALID_IMAGE_NE_FORMAT;
        }

        // MiLegacyImageArchitecture
        bool is_legacy_architecture(const PEMachineType machine)
        {
            return machine == PEMachineType::I386 || machine == PEMachineType::AMD64;
        }

        // MiVerifyImageHeader after the optional header's magic
        template <typename T>
        bool is_valid_optional_header(const PENTHeaders_t<T>& nt_headers)
        {
            constexpr uint16_t required_by_other_architectures = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT;

            const auto& file_header = nt_headers.FileHeader;
            const auto& optional_header = nt_headers.OptionalHeader;
            const auto file_alignment = optional_header.FileAlignment;
            const auto section_alignment = optional_header.SectionAlignment;
            const auto machine = file_header.Machine;
            const auto machine_matches = sizeof(T) == sizeof(uint32_t) ? machine == PEMachineType::I386 || machine == PEMachineType::ARMNT
                                                                       : machine == PEMachineType::AMD64 || machine == PEMachineType::ARM64;
            const auto relocations_stripped = (file_header.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0;
            const auto dll_characteristics = optional_header.DllCharacteristics;
            const auto architecture_allows =
                is_legacy_architecture(machine)
                    ? !relocations_stripped || !(dll_characteristics & IMAGE_DLLCHARACTERISTICS_APPCONTAINER)
                    : !relocations_stripped && (dll_characteristics & required_by_other_architectures) == required_by_other_architectures;

            return (file_alignment % sector_size == 0 || file_alignment == section_alignment) && std::has_single_bit(file_alignment) &&
                   std::has_single_bit(section_alignment) && section_alignment >= file_alignment &&
                   optional_header.SizeOfImage <= largest_image_size && machine_matches &&
                   optional_header.SizeOfHeaders < optional_header.SizeOfImage && optional_header.ImageBase % ALLOCATION_GRANULARITY == 0 &&
                   file_header.SizeOfOptionalHeader % optional_header_size_alignment == 0 && architecture_allows;
        }

        // MiCreateImageFileMap after MiVerifyImageHeader: the headers and the section table have to lie within SizeOfImage, a
        // section table that ends past the header pages read so far is read on its own, and only I386 and AMD64 images may be
        // aligned below a page.
        template <typename T>
        NTSTATUS check_file_map_headers(const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset, const uint64_t read_end,
                                        const uint64_t file_size)
        {
            const auto& file_header = nt_headers.FileHeader;
            const auto& optional_header = nt_headers.OptionalHeader;
            const uint64_t section_table =
                nt_headers_offset + offsetof(PENTHeaders_t<T>, OptionalHeader) + file_header.SizeOfOptionalHeader;
            const auto section_table_end = section_table + uint64_t{file_header.NumberOfSections} * sizeof(IMAGE_SECTION_HEADER);
            if (section_table_end > optional_header.SizeOfImage)
            {
                return STATUS_INVALID_IMAGE_FORMAT;
            }

            if (section_table_end > read_end)
            {
                const auto section_table_page = section_table - section_table % image_header_page_size;
                if (file_header.NumberOfSections == 0 && section_table == section_table_page)
                {
                    return STATUS_INVALID_IMAGE_FORMAT;
                }

                if (section_table_page >= file_size)
                {
                    return STATUS_INVALID_FILE_FOR_SECTION;
                }

                if (section_table_end > file_size)
                {
                    return STATUS_INVALID_IMAGE_FORMAT;
                }
            }

            if (optional_header.SectionAlignment < image_header_page_size && !is_legacy_architecture(file_header.Machine))
            {
                return STATUS_INVALID_IMAGE_FORMAT;
            }

            return STATUS_SUCCESS;
        }

        // MiVerifyImageHeader up to the optional header's magic
        NTSTATUS check_nt_headers(const std::span<const std::byte> first_page, const std::span<const std::byte> new_header,
                                  const uint64_t nt_headers_offset, const uint64_t read_end, const uint64_t file_size)
        {
            const auto nt_headers = read_at<PENTHeaders_t<std::uint32_t>>(new_header, 0);
            if (nt_headers.Signature != PENTHeaders_t<std::uint32_t>::k_Signature)
            {
                return bad_image_header_status(first_page, new_header);
            }

            const auto& file_header = nt_headers.FileHeader;
            if (file_header.Machine == PEMachineType::UNKNOWN && file_header.SizeOfOptionalHeader == 0)
            {
                return STATUS_INVALID_IMAGE_PROTECT;
            }

            if (!(file_header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE))
            {
                return STATUS_INVALID_IMAGE_FORMAT;
            }

            const auto check_headers = [&](const auto& headers) {
                return is_valid_optional_header(headers) ? check_file_map_headers(headers, nt_headers_offset, read_end, file_size)
                                                         : STATUS_INVALID_IMAGE_FORMAT;
            };

            const auto magic = nt_headers.OptionalHeader.Magic;
            if (magic == PEOptionalHeader_t<std::uint32_t>::k_Magic)
            {
                return check_headers(nt_headers);
            }

            if (magic == PEOptionalHeader_t<std::uint64_t>::k_Magic)
            {
                return check_headers(read_at<PENTHeaders_t<std::uint64_t>>(new_header, 0));
            }

            return STATUS_INVALID_IMAGE_FORMAT;
        }

        // 19041's MiCreateImageFileMap up to MiBuildImageControlArea
        NTSTATUS check_image_headers(const std::span<const std::byte> image, const uint64_t file_size)
        {
            if (file_size == 0 || file_size > std::numeric_limits<uint32_t>::max())
            {
                return STATUS_INVALID_FILE_FOR_SECTION;
            }

            const auto first_page = image.first(image_header_page_size);
            const auto dos_header = read_at<PEDosHeader_t>(first_page, 0);
            if (dos_header.e_magic != PEDosHeader_t::k_Magic)
            {
                return STATUS_INVALID_IMAGE_NOT_MZ;
            }

            const uint64_t nt_headers_offset = dos_header.e_lfanew;
            const auto nt_headers_end = nt_headers_offset + sizeof(PENTHeaders_t<std::uint64_t>);
            if (nt_headers_end > file_size || nt_headers_offset % alignof(std::uint32_t) != 0)
            {
                return STATUS_INVALID_IMAGE_PROTECT;
            }

            if (nt_headers_end <= first_page.size())
            {
                return check_nt_headers(first_page, first_page.subspan(static_cast<size_t>(nt_headers_offset)), nt_headers_offset,
                                        first_page.size(), file_size);
            }

            const auto pages_offset = nt_headers_offset - nt_headers_offset % image_header_page_size;
            const auto pages_end =
                pages_offset + std::min<uint64_t>(image_header_later_pages_size, page_align_up(file_size - pages_offset));
            const auto new_header_size = static_cast<size_t>(pages_end - nt_headers_offset);
            const auto read_end = std::min(pages_end, file_size);
            if (pages_end <= file_size)
            {
                return check_nt_headers(first_page, image.subspan(static_cast<size_t>(nt_headers_offset), new_header_size),
                                        nt_headers_offset, read_end, file_size);
            }

            std::array<std::byte, image_header_later_pages_size> new_header{};
            std::ranges::copy(image.subspan(static_cast<size_t>(nt_headers_offset)), new_header.begin());
            return check_nt_headers(first_page, std::span{new_header}.first(new_header_size), nt_headers_offset, read_end, file_size);
        }

        struct section_file_range
        {
            size_t offset{};
            size_t size{};
        };

        section_file_range limit_to_file(const section_file_range range, const uint64_t file_size)
        {
            return {.offset = range.offset,
                    .size = range.offset < file_size ? static_cast<size_t>(std::min<uint64_t>(range.size, file_size - range.offset)) : 0};
        }

        // In a page-aligned image, the kernel reads the data of a section from PointerToRawData rounded down to a
        // 0x200-byte sector up to the end of the raw data rounded up to FileAlignment, limited to the section and the end
        // of the file. Only the pages that start below SizeOfRawData get file data; the kernel fills the others with
        // zeros, as it does for data past the end of the file. Packers such as Upack set PointerToRawData below 0x200 to
        // map the headers into a section.
        section_file_range get_section_file_range(const IMAGE_SECTION_HEADER& section, const uint64_t section_size,
                                                  const uint64_t file_alignment, const uint64_t file_size)
        {
            // The kernel fills a section with PointerToRawData 0 with zeros, whatever its SizeOfRawData.
            if (section.PointerToRawData == 0)
            {
                return {};
            }

            const uint64_t raw_end = static_cast<uint64_t>(section.PointerToRawData) + section.SizeOfRawData;
            const auto start = align_down(section.PointerToRawData, sector_size);
            const auto end = std::min(
                {align_up(raw_end, file_alignment), start + section_size, start + page_align_up(section.SizeOfRawData), file_size});
            return end > start ? section_file_range{.offset = static_cast<size_t>(start), .size = static_cast<size_t>(end - start)}
                               : section_file_range{};
        }

        template <typename T>
        bool map_sections(memory_manager& memory, mapped_module& binary, const utils::safe_buffer_accessor<const std::byte> buffer,
                          const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset)
        {
            const auto first_section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
            const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(first_section_offset));

            for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
            {
                const auto section = sections.get(i);
                const auto target_ptr = binary.image_base + section.VirtualAddress;
                // Like the kernel, only the pages a section uses are committed. With a section alignment above the page
                // size (the 64KB-aligned SysWOW64 DLLs), the rest of its slot stays reserved, which debuggers rely on to
                // tell sections apart in VirtualQuery results.
                const auto virtual_size = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
                const auto size_of_section = page_align_up(virtual_size);
                const auto section_size = static_cast<size_t>(size_of_section);

                if (section_size == 0)
                {
                    continue;
                }

                const auto mapped_as_one_region = is_mapped_as_one_region(nt_headers.OptionalHeader);
                const auto size_of_data = static_cast<size_t>(std::min<uint64_t>(size_of_section, section.SizeOfRawData));
                // A sub-page section's page-rounded size can legitimately run past SizeOfImage; nothing is committed or
                // protected per section there, so only the data written must fit.
                const auto size_in_image = mapped_as_one_region ? size_of_data : section_size;
                if (section.VirtualAddress + size_in_image > binary.size_of_image)
                {
                    throw std::runtime_error("Section extends beyond the image");
                }

                if (!mapped_as_one_region && !memory.commit_image_memory(target_ptr, section_size, memory_permission::read_write))
                {
                    return false;
                }

                if (section.SizeOfRawData > 0)
                {
                    const uint64_t file_size = buffer.get_buffer().size();
                    const auto range =
                        mapped_as_one_region
                            ? limit_to_file({.offset = section.PointerToRawData, .size = size_of_data}, file_size)
                            : get_section_file_range(section, size_of_section, nt_headers.OptionalHeader.FileAlignment, file_size);
                    if (range.size)
                    {
                        memory.write_memory(target_ptr, buffer.get_pointer_for_range(range.offset, range.size), range.size);
                    }
                }

                const auto permissions = get_section_permissions(section, nt_headers.FileHeader.Machine);

                mapped_section section_info{};
                section_info.region.start = target_ptr;
                section_info.region.length = section_size;
                section_info.region.permissions = permissions;

                for (size_t j = 0; j < sizeof(section.Name) && section.Name[j]; ++j)
                {
                    section_info.name.push_back(static_cast<char>(section.Name[j]));
                }

                binary.sections.push_back(std::move(section_info));
            }

            return true;
        }

        bool protect_module_memory(memory_manager& memory, const mapped_module& binary, const size_t headers_size)
        {
            if (!memory.protect_memory(binary.image_base, headers_size, memory_permission::read))
            {
                return false;
            }

            for (const auto& section : binary.sections)
            {
                if (!memory.protect_memory(section.region.start, section.region.length, section.region.permissions, nullptr))
                {
                    return false;
                }
            }

            return true;
        }

        template <typename T>
        bool try_map_module_at_current_base(memory_manager& memory, mapped_module& binary,
                                            const utils::safe_buffer_accessor<const std::byte> buffer, const PENTHeaders_t<T>& nt_headers,
                                            const uint64_t nt_headers_offset, const PEOptionalHeader_t<T>& optional_header,
                                            const uint64_t relocation_base)
        {
            binary.sections.clear();
            binary.exports.clear();
            binary.imports.clear();
            binary.imported_modules.clear();
            binary.address_names.clear();
            binary.pdb.reset();
            binary.pe_sections.clear();

            const auto image_size = static_cast<size_t>(binary.size_of_image);

            if (!memory.allocate_memory(binary.image_base, image_size, memory_permission::all, true, memory_region_kind::section_image))
            {
                return false;
            }

            const auto mapped_as_one_region = is_mapped_as_one_region(optional_header);
            const auto headers_size = static_cast<size_t>(page_align_up(optional_header.SizeOfHeaders));
            const auto initial_commit_size = mapped_as_one_region ? image_size : headers_size;
            if (!memory.commit_image_memory(binary.image_base, initial_commit_size, memory_permission::read_write))
            {
                memory.release_memory(binary.image_base, 0);
                return false;
            }

            try
            {
                const auto* header_buffer = buffer.get_pointer_for_range(0, optional_header.SizeOfHeaders);
                memory.write_memory(binary.image_base, header_buffer, optional_header.SizeOfHeaders);

                if (!map_sections(memory, binary, buffer, nt_headers, nt_headers_offset))
                {
                    memory.release_memory(binary.image_base, 0);
                    binary.sections.clear();
                    return false;
                }

                const auto image_base = static_cast<T>(relocation_base);
                const auto image_base_address = binary.image_base + nt_headers_offset + offsetof(PENTHeaders_t<T>, OptionalHeader) +
                                                offsetof(PEOptionalHeader_t<T>, ImageBase);
                memory.write_memory(image_base_address, &image_base, sizeof(image_base));

                apply_relocations(binary, memory, optional_header, relocation_base);
                collect_exports_or_ignore(binary, memory, optional_header);
                collect_imports(binary, memory, optional_header);

                // TODO: Make sure to match kernel allocation patterns to attain correct initial permissions!
                const auto protected_ok = mapped_as_one_region
                                              ? memory.protect_memory(binary.image_base, image_size, memory_permission::all)
                                              : protect_module_memory(memory, binary, headers_size);
                if (!protected_ok)
                {
                    throw std::runtime_error("Failed to protect mapped module memory");
                }
            }
            catch (...)
            {
                memory.release_memory(binary.image_base, 0);

                binary.sections.clear();
                binary.exports.clear();
                binary.imports.clear();
                binary.imported_modules.clear();
                binary.address_names.clear();
                binary.pdb.reset();
                binary.pe_sections.clear();
                throw;
            }

            return true;
        }
    }

    template <typename T>
    mapped_module map_module_from_data(memory_manager& memory, const std::span<const std::byte> data, std::filesystem::path file,
                                       windows_path module_path, const uint64_t relocation_base, const uint64_t highest_address,
                                       NTSTATUS* status)
    {
        mapped_module binary{};
        binary.path = std::move(file);
        binary.name = u16_to_u8(module_path.leaf());
        binary.module_path = std::move(module_path);

        utils::safe_buffer_accessor buffer{data};

        const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
        const auto nt_headers_offset = dos_header.e_lfanew;
        const auto nt_headers = read_nt_headers<T>(buffer, nt_headers_offset);
        const auto& optional_header = nt_headers.OptionalHeader;

        validate_image_machine(nt_headers);
        validate_image_layout(buffer, nt_headers, nt_headers_offset, data.size());

        binary.image_base = optional_header.ImageBase;
        binary.image_base_file = optional_header.ImageBase;
        binary.size_of_image = page_align_up(optional_header.SizeOfImage); // TODO: Sanitize

        const bool force_wow64cpu_32bit_va = must_map_module_below_4gb(binary.name, nt_headers.FileHeader.Machine);
        constexpr uint64_t below_4gb_ceiling = 0xFFFFFFFFULL;
        const bool is_32bit = (nt_headers.FileHeader.Machine == PEMachineType::I386);
        const auto architecture_limit = is_32bit || force_wow64cpu_32bit_va ? below_4gb_ceiling : MAX_ALLOCATION_ADDRESS;
        const auto address_limit = std::min(highest_address ? highest_address : MAX_ALLOCATION_ADDRESS, architecture_limit);

        if (force_wow64cpu_32bit_va && binary.image_base > below_4gb_ceiling)
        {
            binary.image_base = memory.find_free_host_allocation_base(static_cast<size_t>(binary.size_of_image),
                                                                      DEFAULT_ALLOCATION_ADDRESS_32BIT, address_limit);
        }

        // Store PE header fields
        binary.machine = static_cast<uint16_t>(nt_headers.FileHeader.Machine);
        binary.characteristics = nt_headers.FileHeader.Characteristics;
        binary.dll_characteristics = optional_header.DllCharacteristics;
        binary.subsystem_major_version = optional_header.MajorSubsystemVersion;
        binary.size_of_stack_reserve = optional_header.SizeOfStackReserve;
        binary.size_of_stack_commit = optional_header.SizeOfStackCommit;
        binary.size_of_heap_reserve = optional_header.SizeOfHeapReserve;
        binary.size_of_heap_commit = optional_header.SizeOfHeapCommit;
        collect_pe_sections(binary, buffer, nt_headers, nt_headers_offset);
        collect_pdb_signature(binary, buffer, nt_headers, nt_headers_offset, false);

        const auto is_dll = nt_headers.FileHeader.Characteristics & IMAGE_FILE_DLL;
        const auto has_dynamic_base = optional_header.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
        const auto relocations_stripped = nt_headers.FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED;
        const auto is_relocatable = (is_dll || has_dynamic_base) && !relocations_stripped;

        const auto is_x64 = nt_headers.FileHeader.Machine == PEMachineType::AMD64;
        const bool prefers_below_4gb = is_x64 && is_dll && optional_header.ImageBase <= below_4gb_ceiling;
        const auto& relocations = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        const bool has_relocations = relocations.Size != 0 && !(nt_headers.FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED);
        if (!relocation_base && !force_wow64cpu_32bit_va && !prefers_below_4gb && (is_32bit || is_x64) && is_dll && has_dynamic_base &&
            has_relocations)
        {
            const uint64_t image_arena_start = is_32bit ? 0x70000000ULL : 0x00007FF000000000ULL;
            const uint64_t image_arena_end = std::min(is_32bit ? 0x7FFDFFFFULL : MAX_ALLOCATION_ADDRESS, address_limit);
            binary.image_base =
                image_arena_start <= image_arena_end
                    ? memory.find_free_host_allocation_base(static_cast<size_t>(binary.size_of_image), image_arena_start, image_arena_end)
                    : 0;
        }

        const auto preferred_base = relocation_base ? relocation_base : binary.image_base;
        const bool preferred_fits = binary.image_base && binary.size_of_image && binary.image_base <= address_limit &&
                                    binary.size_of_image - 1 <= address_limit - binary.image_base;
        if (!preferred_fits || !try_map_module_at_current_base(memory, binary, buffer, nt_headers, nt_headers_offset, optional_header,
                                                               relocation_base ? relocation_base : binary.image_base))
        {
            if (!is_relocatable && relocation_base == 0)
            {
                throw std::bad_alloc{};
            }

            // The ceiling is explicit because an unbounded search can pick a base above 4 GB for a
            // 32-bit module once the low arena fills; WOW64 pointer marshaling then truncates it to 32
            // bits, aliasing the module onto whatever unrelated allocation sits at the truncated address.
            const bool needs_below_4gb = address_limit < DEFAULT_ALLOCATION_ADDRESS_64BIT;
            // Windows prefers low addresses for low-based PE64 images because they may truncate pointers.
            uint64_t fallback_start =
                needs_below_4gb || prefers_below_4gb ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
            const auto image_size = static_cast<size_t>(binary.size_of_image);

            // The preferred base was taken, so relocate. On backends sharing the address space with the
            // guest (FEX on Apple Silicon), a foreign host mapping can occupy a VA sogen still believes
            // is free, and a single pick would fail the map outright even though other addresses are
            // available. A failed try_map_module_at_current_base records the intruding host range via the
            // fixed-address allocate_memory's windowed rescan, so re-picking steps past it; bounded so an
            // exhausted address space terminates rather than spins.
            constexpr int max_host_relocation_retries = 8;
            bool mapped = false;
            for (int attempt = 0; attempt <= max_host_relocation_retries; ++attempt)
            {
                binary.image_base = memory.find_free_host_allocation_base(image_size, fallback_start, address_limit);
                if (!binary.image_base && fallback_start == DEFAULT_ALLOCATION_ADDRESS_64BIT)
                {
                    fallback_start = DEFAULT_ALLOCATION_ADDRESS_32BIT;
                    binary.image_base = memory.find_free_host_allocation_base(image_size, fallback_start, address_limit);
                }
                if (!binary.image_base)
                {
                    break;
                }

                // A secondary view may occupy another address while its relocation fixups
                // continue to reference the original loaded image.
                if (try_map_module_at_current_base(memory, binary, buffer, nt_headers, nt_headers_offset, optional_header,
                                                   relocation_base ? relocation_base : binary.image_base))
                {
                    mapped = true;
                    break;
                }
            }

            if (!mapped)
            {
                throw std::bad_alloc{};
            }
        }

        collect_pe_sections(binary, buffer, nt_headers, nt_headers_offset);
        collect_pdb_signature(binary, buffer, nt_headers, nt_headers_offset, false);

        binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;
        if (status)
        {
            *status = binary.image_base == preferred_base ? STATUS_SUCCESS : STATUS_IMAGE_NOT_AT_BASE;
        }

        return binary;
    }

    template <typename T>
    mapped_module map_module_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size, windows_path module_path)
    {
        mapped_module binary{};
        binary.name = u16_to_u8(module_path.leaf());
        // mapped_module::path is the module identity the load counts, the duplicate checks and the loaded-module
        // comparisons key on, so it stays canonical and lower case even though the guest path keeps its spelling.
        binary.path = std::filesystem::path{utils::string::to_lower(module_path.lexically_normal().to_portable_path().u16string())};
        binary.module_path = std::move(module_path);
        binary.image_base = base_address;
        binary.image_base_file = base_address;
        binary.size_of_image = image_size;

        auto mapped_memory = read_mapped_memory<T>(memory, binary);
        utils::safe_buffer_accessor<const std::byte> buffer{mapped_memory};

        try
        {
            const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
            const auto nt_headers_offset = dos_header.e_lfanew;
            const auto nt_headers = buffer.as<PENTHeaders_t<T>>(nt_headers_offset).get();
            validate_image_machine(nt_headers);
            const auto& optional_header = nt_headers.OptionalHeader;

            binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;

            // Store PE header fields
            binary.machine = static_cast<uint16_t>(nt_headers.FileHeader.Machine);
            binary.characteristics = nt_headers.FileHeader.Characteristics;
            binary.dll_characteristics = optional_header.DllCharacteristics;
            binary.subsystem_major_version = optional_header.MajorSubsystemVersion;
            binary.size_of_stack_reserve = optional_header.SizeOfStackReserve;
            binary.size_of_stack_commit = optional_header.SizeOfStackCommit;
            binary.size_of_heap_reserve = optional_header.SizeOfHeapReserve;
            binary.size_of_heap_commit = optional_header.SizeOfHeapCommit;
            collect_pe_sections(binary, buffer, nt_headers, nt_headers_offset);
            collect_pdb_signature(binary, buffer, nt_headers, nt_headers_offset, true);

            const auto section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
            const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(section_offset));

            for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
            {
                const auto section = sections.get(i);

                mapped_section section_info{};
                section_info.region.start = binary.image_base + section.VirtualAddress;
                section_info.region.length = static_cast<size_t>(page_align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize)));

                const auto permissions = get_section_permissions(section, nt_headers.FileHeader.Machine);

                section_info.region.permissions = permissions;

                for (size_t j = 0; j < sizeof(section.Name) && section.Name[j]; ++j)
                {
                    section_info.name.push_back(static_cast<char>(section.Name[j]));
                }

                binary.sections.push_back(std::move(section_info));
            }

            collect_exports_or_ignore(binary, buffer, optional_header);
        }
        catch (const std::exception&)
        {
            // bad!
            throw std::runtime_error("Failed to map module from memory at " + std::to_string(base_address) + " with size " +
                                     std::to_string(image_size) + " for module " + binary.name);
        }

        return binary;
    }

    namespace
    {
        NTSTATUS check_layout(const std::span<const std::byte> headers, const uint64_t file_size)
        {
            try
            {
                const utils::safe_buffer_accessor<const std::byte> buffer{headers};
                const auto nt_headers_offset = buffer.as<PEDosHeader_t>(0).get().e_lfanew;
                const auto magic = buffer.as<uint16_t>(nt_headers_offset + offsetof(PENTHeaders_t<std::uint32_t>, OptionalHeader)).get();
                if (magic == PEOptionalHeader_t<std::uint32_t>::k_Magic)
                {
                    validate_image_layout(buffer, read_nt_headers<std::uint32_t>(buffer, nt_headers_offset), nt_headers_offset, file_size);
                }
                else if (magic == PEOptionalHeader_t<std::uint64_t>::k_Magic)
                {
                    validate_image_layout(buffer, read_nt_headers<std::uint64_t>(buffer, nt_headers_offset), nt_headers_offset, file_size);
                }
            }
            catch (const invalid_image_format& e)
            {
                return e.status;
            }
            catch (const std::exception&)
            {
                // Unreadable headers are reported when the image is mapped.
            }

            return STATUS_SUCCESS;
        }
    }

    NTSTATUS check_image(const std::span<const std::byte> data)
    {
        const auto check = [&](const std::span<const std::byte> image) {
            const auto status = check_image_headers(image, data.size());
            return status == STATUS_SUCCESS ? check_layout(image, data.size()) : status;
        };

        // MiCreateImageFileMap reads the headers of a file smaller than a page as a page that is zero past the file's end.
        if (data.size() < image_header_page_size)
        {
            std::array<std::byte, image_header_page_size> first_page{};
            std::ranges::copy(data, first_page.begin());
            return check(first_page);
        }

        return check(data);
    }

    bool unmap_module(memory_manager& memory, const mapped_module& mod)
    {
        if (memory.release_memory(mod.image_base, 0))
        {
            return true;
        }

        std::unordered_set<uint64_t> released_bases{};

        bool success = true;

        for (const auto& section : mod.sections)
        {
            if (released_bases.emplace(section.region.start).second)
            {
                success = memory.release_memory(section.region.start, 0) && success;
            }
        }

        return success;
    }

    template mapped_module map_module_from_data<std::uint32_t>(memory_manager& memory, const std::span<const std::byte> data,
                                                               std::filesystem::path file, windows_path module_path,
                                                               uint64_t relocation_base, uint64_t highest_address, NTSTATUS* status);
    template mapped_module map_module_from_data<std::uint64_t>(memory_manager& memory, const std::span<const std::byte> data,
                                                               std::filesystem::path file, windows_path module_path,
                                                               uint64_t relocation_base, uint64_t highest_address, NTSTATUS* status);

    template mapped_module map_module_from_memory<std::uint32_t>(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                                 windows_path module_path);
    template mapped_module map_module_from_memory<std::uint64_t>(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                                 windows_path module_path);

} // namespace sogen
