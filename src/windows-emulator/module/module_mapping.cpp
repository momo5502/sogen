#include "../std_include.hpp"
#include "module_mapping.hpp"
#include <address_utils.hpp>
#include <algorithm>

#include <utils/io.hpp>
#include <utils/buffer_accessor.hpp>
#include <utils/string.hpp>
#include <platform/win_pefile.hpp>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-template"
#endif

namespace sogen
{

    namespace
    {
        bool must_map_module_below_4gb(const std::string& module_name, const PEMachineType machine, const uint64_t image_base)
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

            return image_base > std::numeric_limits<uint32_t>::max();
        }

        template <typename T>
        std::vector<std::byte> read_mapped_memory(const memory_manager& memory, const mapped_module& binary)
        {
            std::vector<std::byte> mem{};
            mem.resize(static_cast<size_t>(binary.size_of_image));
            memory.read_memory(binary.image_base, mem.data(), mem.size());

            return mem;
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

            const auto names = buffer.as<DWORD>(export_directory.AddressOfNames);
            const auto ordinals = buffer.as<WORD>(export_directory.AddressOfNameOrdinals);
            const auto functions = buffer.as<DWORD>(export_directory.AddressOfFunctions);

            binary.exports.reserve(function_count);

            // Walk every ordinal slot (AddressOfFunctions), not just the named ones
            // (AddressOfNames/AddressOfNameOrdinals) - a DLL can export a function by ordinal only,
            // with no name at all (common for internal/undocumented APIs, e.g. wow64cpu.dll's own
            // low-level CPU-simulation entry points), and such an export must still be resolvable
            // when something imports it by ordinal (see collect_imports's snap_by_ordinal path).
            for (DWORD ordinal = 0; ordinal < function_count; ordinal++)
            {
                const auto rva = functions.get(ordinal);
                if (rva == 0)
                {
                    continue; // Empty slot in a sparse ordinal range - not a real export.
                }

                exported_symbol symbol{};
                symbol.ordinal = export_directory.Base + ordinal;
                symbol.rva = rva;
                symbol.address = binary.image_base + symbol.rva;

                for (DWORD i = 0; i < names_count; i++)
                {
                    if (ordinals.get(i) == static_cast<WORD>(ordinal))
                    {
                        symbol.name = buffer.as_string(names.get(i));
                        break;
                    }
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
                using thunk_type = typename thunk_traits::type;

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
                        const auto by_name_address =
                            binary.image_base + original_thunk.u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name);
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
            binary.exports.reserve(names_count);

            for (DWORD i = 0; i < names_count; i++)
            {
                const auto ordinal =
                    read_mapped_object<WORD>(memory, binary.image_base + export_directory.AddressOfNameOrdinals + i * sizeof(WORD));
                const auto function_rva =
                    read_mapped_object<DWORD>(memory, binary.image_base + export_directory.AddressOfFunctions + ordinal * sizeof(DWORD));
                const auto name_rva =
                    read_mapped_object<DWORD>(memory, binary.image_base + export_directory.AddressOfNames + i * sizeof(DWORD));

                exported_symbol symbol{};
                symbol.ordinal = export_directory.Base + ordinal;
                symbol.rva = function_rva;
                symbol.address = binary.image_base + symbol.rva;
                symbol.name = read_mapped_string(memory, binary.image_base + name_rva);

                binary.exports.push_back(std::move(symbol));
            }

            for (const auto& symbol : binary.exports)
            {
                binary.address_names.try_emplace(symbol.address, symbol.name);
            }
        }

        // Applies one relocation block's fixups against a host-side mirror of the block's guest byte
        // span, then commits the whole span with a single memory.write_memory call. A freshly-mapped
        // module has not executed a single instruction yet at this point in the load sequence (this
        // runs before protect_module_memory and before the module's entry point), so - unlike the
        // general write_memory caller population, which can legitimately alias code (e.g. a syscall
        // output buffer) - every one of these writes is provably invalidating a range that cannot yet
        // hold a JIT translation. Reading, fixing up, and writing the whole block as one range still
        // performs that (harmless-here) invalidation, but once per block instead of once per fixup,
        // collapsing what can be thousands of individually-invalidating guest writes per module down
        // to one per relocation block (typically ~one page's worth of fixups).
        template <typename T>
        void apply_relocation_block(memory_manager& memory, const uint64_t image_base, const IMAGE_BASE_RELOCATION& relocation,
                                    const std::span<const uint16_t> entries, const uint64_t delta)
        {
            uint64_t min_offset = 0;
            uint64_t max_end = 0;
            bool has_fixup = false;

            for (const auto entry : entries)
            {
                const int type = entry >> 12;
                const auto offset = static_cast<uint16_t>(entry & 0xfff);
                const auto total_offset = static_cast<uint64_t>(relocation.VirtualAddress) + offset;

                size_t width = 0;
                switch (type)
                {
                case IMAGE_REL_BASED_ABSOLUTE:
                    continue;
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
            std::vector<std::byte> block_buffer(span_size);
            memory.read_memory(image_base + min_offset, block_buffer.data(), span_size);

            const utils::safe_buffer_accessor<std::byte> block{block_buffer};

            for (const auto entry : entries)
            {
                const int type = entry >> 12;
                const auto offset = static_cast<uint16_t>(entry & 0xfff);
                const auto total_offset = static_cast<uint64_t>(relocation.VirtualAddress) + offset;
                const auto local_offset = total_offset - min_offset;

                switch (type)
                {
                case IMAGE_REL_BASED_ABSOLUTE:
                    break;

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

                // Read every entry descriptor for this block in one guest read instead of one per entry.
                entries.resize(entry_count);
                if (entry_count > 0)
                {
                    memory.read_memory(entries_base, entries.data(), entry_count * sizeof(uint16_t));
                }

                apply_relocation_block<T>(memory, binary.image_base, relocation, entries, delta);
            }
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
                const auto size_of_section = page_align_up(section.Misc.VirtualSize, nt_headers.OptionalHeader.SectionAlignment);
                const auto section_size = static_cast<size_t>(size_of_section);

                if (section_size == 0)
                {
                    continue;
                }

                if (!memory.commit_image_memory(target_ptr, section_size, memory_permission::read_write))
                {
                    return false;
                }

                if (section.SizeOfRawData > 0)
                {
                    const auto size_of_data = static_cast<size_t>(std::min<uint64_t>(size_of_section, section.SizeOfRawData));
                    const auto* source_ptr = buffer.get_pointer_for_range(section.PointerToRawData, size_of_data);
                    memory.write_memory(target_ptr, source_ptr, size_of_data);
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

            const auto image_size = static_cast<size_t>(binary.size_of_image);
            if (!memory.allocate_memory(binary.image_base, image_size, memory_permission::all, true, memory_region_kind::section_image))
            {
                return false;
            }

            const auto headers_size = static_cast<size_t>(page_align_up(optional_header.SizeOfHeaders));
            if (!memory.commit_image_memory(binary.image_base, headers_size, memory_permission::read_write))
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
                collect_exports(binary, memory, optional_header);
                collect_imports(binary, memory, optional_header);

                // TODO: Make sure to match kernel allocation patterns to attain correct initial permissions!
                if (!protect_module_memory(memory, binary, headers_size))
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
                throw;
            }

            return true;
        }
    }

    template <typename T>
    mapped_module map_module_from_data(memory_manager& memory, const std::span<const std::byte> data, std::filesystem::path file,
                                       windows_path module_path, const uint64_t relocation_base)
    {
        mapped_module binary{};
        binary.path = std::move(file);
        binary.name = u16_to_u8(module_path.leaf());
        binary.module_path = std::move(module_path);

        utils::safe_buffer_accessor buffer{data};

        const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
        const auto nt_headers_offset = dos_header.e_lfanew;

        const auto nt_headers = buffer.as<PENTHeaders_t<T>>(nt_headers_offset).get();
        const auto& optional_header = nt_headers.OptionalHeader;

        if (nt_headers.FileHeader.Machine != PEMachineType::I386 && nt_headers.FileHeader.Machine != PEMachineType::AMD64)
        {
            throw std::runtime_error("Unsupported architecture!");
        }

        binary.image_base = optional_header.ImageBase;
        binary.image_base_file = optional_header.ImageBase;
        binary.size_of_image = page_align_up(optional_header.SizeOfImage); // TODO: Sanitize

        const bool force_wow64cpu_32bit_va = must_map_module_below_4gb(binary.name, nt_headers.FileHeader.Machine, binary.image_base);

        if (force_wow64cpu_32bit_va)
        {
            binary.image_base =
                memory.find_free_allocation_base(static_cast<size_t>(binary.size_of_image), DEFAULT_ALLOCATION_ADDRESS_32BIT);
        }

        // Store PE header fields
        binary.machine = static_cast<uint16_t>(nt_headers.FileHeader.Machine);
        binary.dll_characteristics = optional_header.DllCharacteristics;
        binary.size_of_stack_reserve = optional_header.SizeOfStackReserve;
        binary.size_of_stack_commit = optional_header.SizeOfStackCommit;
        binary.size_of_heap_reserve = optional_header.SizeOfHeapReserve;
        binary.size_of_heap_commit = optional_header.SizeOfHeapCommit;

        const bool is_32bit = (nt_headers.FileHeader.Machine == PEMachineType::I386);
        const auto is_dll = nt_headers.FileHeader.Characteristics & IMAGE_FILE_DLL;
        const auto has_dynamic_base = optional_header.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
        const auto is_relocatable = is_dll || has_dynamic_base;

        if (!binary.image_base || !try_map_module_at_current_base(memory, binary, buffer, nt_headers, nt_headers_offset, optional_header,
                                                                  relocation_base ? relocation_base : binary.image_base))
        {
            if (!is_relocatable && relocation_base == 0)
            {
                throw std::runtime_error("Memory range not allocatable");
            }

            // 32-bit (WOW64) modules must stay below 4 GB; native modules use the 64-bit arena.
            const bool needs_below_4gb = force_wow64cpu_32bit_va || is_32bit;
            const uint64_t fallback_start = needs_below_4gb ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
            // find_free_host_allocation_base's 2-arg overload has no ceiling at all (MAX_ALLOCATION_ADDRESS,
            // effectively the top of the host-addressable range) - for a 32-bit module this can silently
            // return an address above 4GB once the low arena fragments/fills (e.g. after enough
            // load/unload churn forces repeated relocation), which then gets truncated to 32 bits by
            // guest/WOW64 pointer marshaling and aliases onto whatever unrelated low allocation happens to
            // sit at the truncated address - confirmed via CI as the root cause of a deterministic access
            // violation inside ntdll during wow64-test-sample.exe's LoadLibraryA/FreeLibrary churn (a
            // stable ntdll stack slot got silently overwritten). The heaven's-gate and native-wow64-stack
            // call sites already learned this and pass an explicit below-4GB ceiling; this relocation
            // fallback for 32-bit modules needed the same fix.
            constexpr uint64_t below_4gb_ceiling = 0xFFFFFFFFULL;
            const uint64_t highest_address = needs_below_4gb ? below_4gb_ceiling : MAX_ALLOCATION_ADDRESS;
            const auto image_size = static_cast<size_t>(binary.size_of_image);

            // The preferred base was taken, so relocate. find_free_host_allocation_base picks a base and
            // confirms it is actually free at the host level, not merely per sogen's own bookkeeping. That
            // matters on backends sharing the guest address space with the host process (FEX on Apple
            // Silicon: guest VA == host VA), where a foreign host mapping (a lazily-loaded dylib, a thread
            // stack, ASLR-placed anything) can occupy a VA sogen still believes is free. The old code picked
            // once via find_free_allocation_base and retried the map exactly once, so a single such collision
            // threw "Memory range not allocatable" outright. The loop below re-picks past a racer instead:
            // a failed try_map_module_at_current_base has already recorded the intruding host range (via the
            // fixed-address allocate_memory's windowed rescan), so the next pick steps past it. Bounded so a
            // genuinely exhausted address space still terminates rather than spinning.
            constexpr int max_host_relocation_retries = 8;
            bool mapped = false;
            // The free-pick retry loop only makes sense when the caller left the target address up to
            // us (relocation_base == 0) - if the caller specified a real target (mapping a view of an
            // already-loaded image at that image's own base, so the view's internal absolute pointers
            // stay correct), picking a different free host address instead would silently relocate the
            // view away from where the caller actually needs it.
            if (relocation_base == 0)
            {
                for (int attempt = 0; attempt <= max_host_relocation_retries; ++attempt)
                {
                    binary.image_base = memory.find_free_host_allocation_base(image_size, fallback_start, highest_address);
                    if (!binary.image_base)
                    {
                        break;
                    }

                    if (try_map_module_at_current_base(memory, binary, buffer, nt_headers, nt_headers_offset, optional_header,
                                                       binary.image_base))
                    {
                        mapped = true;
                        break;
                    }
                }
            }

            if (!mapped && (!binary.image_base ||
                            !try_map_module_at_current_base(memory, binary, buffer, nt_headers, nt_headers_offset, optional_header,
                                                            relocation_base ? relocation_base : binary.image_base)))
            {
                throw std::runtime_error("Memory range not allocatable");
            }
        }

        binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;

        return binary;
    }

    template <typename T>
    mapped_module map_module_from_file(memory_manager& memory, std::filesystem::path file, windows_path module_path,
                                       const uint64_t relocation_base)
    {
        const auto data = utils::io::read_file(file);
        if (data.empty())
        {
            throw std::runtime_error("Bad file data: " + file.string());
        }

        return map_module_from_data<T>(memory, data, std::move(file), std::move(module_path), relocation_base);
    }

    template <typename T>
    mapped_module map_module_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size, windows_path module_path)
    {
        mapped_module binary{};
        binary.name = u16_to_u8(module_path.leaf());
        binary.path = module_path.to_portable_path();
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
            const auto nt_headers = buffer.as<PENTHeaders_t<std::uint64_t>>(nt_headers_offset).get();
            const auto& optional_header = nt_headers.OptionalHeader;

            binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;

            // Store PE header fields
            binary.machine = static_cast<uint16_t>(nt_headers.FileHeader.Machine);
            binary.dll_characteristics = optional_header.DllCharacteristics;
            binary.size_of_stack_reserve = optional_header.SizeOfStackReserve;
            binary.size_of_stack_commit = optional_header.SizeOfStackCommit;
            binary.size_of_heap_reserve = optional_header.SizeOfHeapReserve;
            binary.size_of_heap_commit = optional_header.SizeOfHeapCommit;

            const auto section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
            const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(section_offset));

            for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
            {
                const auto section = sections.get(i);

                mapped_section section_info{};
                section_info.region.start = binary.image_base + section.VirtualAddress;
                section_info.region.length = static_cast<size_t>(page_align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize)));

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

                section_info.region.permissions = permissions;

                for (size_t j = 0; j < sizeof(section.Name) && section.Name[j]; ++j)
                {
                    section_info.name.push_back(static_cast<char>(section.Name[j]));
                }

                binary.sections.push_back(std::move(section_info));
            }

            collect_exports(binary, buffer, optional_header);
        }
        catch (const std::exception&)
        {
            // bad!
            throw std::runtime_error("Failed to map module from memory at " + std::to_string(base_address) + " with size " +
                                     std::to_string(image_size) + " for module " + binary.name);
        }

        return binary;
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
                                                               uint64_t relocation_base);
    template mapped_module map_module_from_data<std::uint64_t>(memory_manager& memory, const std::span<const std::byte> data,
                                                               std::filesystem::path file, windows_path module_path,
                                                               uint64_t relocation_base);
    template mapped_module map_module_from_file<std::uint32_t>(memory_manager& memory, std::filesystem::path file, windows_path module_path,
                                                               uint64_t relocation_base);
    template mapped_module map_module_from_file<std::uint64_t>(memory_manager& memory, std::filesystem::path file, windows_path module_path,
                                                               uint64_t relocation_base);

    template mapped_module map_module_from_memory<std::uint32_t>(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                                 windows_path module_path);
    template mapped_module map_module_from_memory<std::uint64_t>(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                                 windows_path module_path);

} // namespace sogen
