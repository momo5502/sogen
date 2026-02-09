#include "../std_include.hpp"
#include "elf_mapping.hpp"

#include <algorithm>
#include <cstring>

using namespace elf;

namespace
{
    std::string read_strtab_entry(const std::span<const std::byte> data, uint64_t strtab_offset, uint32_t name_index)
    {
        const auto offset = strtab_offset + name_index;
        if (offset >= data.size())
        {
            return {};
        }

        const auto* str = reinterpret_cast<const char*>(data.data() + offset);
        const auto max_len = data.size() - offset;

        size_t len = 0;
        while (len < max_len && str[len] != '\0')
        {
            ++len;
        }

        return std::string(str, len);
    }

    void parse_dynamic_section(const std::span<const std::byte> data, const Elf64_Phdr& dyn_phdr, linux_mapped_module& mod,
                               uint64_t /*base_delta*/)
    {
        if (dyn_phdr.p_offset + dyn_phdr.p_filesz > data.size())
        {
            return;
        }

        const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(data.data() + dyn_phdr.p_offset);
        const auto count = dyn_phdr.p_filesz / sizeof(Elf64_Dyn);

        // First pass: find DT_STRTAB
        uint64_t strtab_vaddr = 0;
        for (size_t i = 0; i < count; ++i)
        {
            if (dyn[i].d_tag == DT_STRTAB)
            {
                strtab_vaddr = dyn[i].d_un.d_ptr;
                break;
            }

            if (dyn[i].d_tag == DT_NULL)
            {
                break;
            }
        }

        // Convert strtab vaddr to file offset
        // For ET_EXEC, strtab_vaddr is an absolute VA
        // For ET_DYN, strtab_vaddr is relative to load base
        uint64_t strtab_file_offset = 0;
        if (strtab_vaddr != 0)
        {
            const auto* ehdr = get_header(data);
            const auto* phdrs = get_program_headers(data);

            for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
            {
                const auto& ph = phdrs[i];
                if (ph.p_type != PT_LOAD)
                {
                    continue;
                }

                const auto seg_vaddr = ph.p_vaddr;
                if (strtab_vaddr >= seg_vaddr && strtab_vaddr < seg_vaddr + ph.p_filesz)
                {
                    strtab_file_offset = ph.p_offset + (strtab_vaddr - seg_vaddr);
                    break;
                }
            }
        }

        // Second pass: read DT_NEEDED, DT_RPATH, DT_RUNPATH entries
        for (size_t i = 0; i < count; ++i)
        {
            if (dyn[i].d_tag == DT_NULL)
            {
                break;
            }

            if (strtab_file_offset == 0)
            {
                continue;
            }

            if (dyn[i].d_tag == DT_NEEDED)
            {
                auto name = read_strtab_entry(data, strtab_file_offset, static_cast<uint32_t>(dyn[i].d_un.d_val));
                if (!name.empty())
                {
                    mod.needed_libraries.push_back(std::move(name));
                }
            }
            else if (dyn[i].d_tag == DT_RPATH)
            {
                mod.rpath = read_strtab_entry(data, strtab_file_offset, static_cast<uint32_t>(dyn[i].d_un.d_val));
            }
            else if (dyn[i].d_tag == DT_RUNPATH)
            {
                mod.runpath = read_strtab_entry(data, strtab_file_offset, static_cast<uint32_t>(dyn[i].d_un.d_val));
            }
        }
    }

    void parse_symbols(const std::span<const std::byte> data, linux_mapped_module& mod, uint64_t base)
    {
        const auto* ehdr = get_header(data);
        const auto* shdrs = get_section_headers(data);
        if (!shdrs)
        {
            return;
        }

        for (uint16_t i = 0; i < ehdr->e_shnum; ++i)
        {
            const auto& sh = shdrs[i];

            if (sh.sh_type != SHT_DYNSYM && sh.sh_type != SHT_SYMTAB)
            {
                continue;
            }

            if (sh.sh_offset + sh.sh_size > data.size())
            {
                continue;
            }

            // Get the associated string table
            if (sh.sh_link >= ehdr->e_shnum)
            {
                continue;
            }

            const auto& strtab_sh = shdrs[sh.sh_link];
            if (strtab_sh.sh_offset + strtab_sh.sh_size > data.size())
            {
                continue;
            }

            const auto sym_count = sh.sh_size / sizeof(Elf64_Sym);
            const auto* syms = reinterpret_cast<const Elf64_Sym*>(data.data() + sh.sh_offset);

            for (size_t j = 1; j < sym_count; ++j) // skip index 0 (undefined)
            {
                const auto& sym = syms[j];

                if (sym.st_shndx == SHN_UNDEF)
                {
                    continue;
                }

                const auto bind = elf64_st_bind(sym.st_info);
                const auto type = elf64_st_type(sym.st_info);

                if (bind != STB_GLOBAL && bind != STB_WEAK)
                {
                    continue;
                }

                if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)
                {
                    continue;
                }

                auto name = read_strtab_entry(data, strtab_sh.sh_offset, sym.st_name);
                if (name.empty())
                {
                    continue;
                }

                linux_exported_symbol exp{};
                exp.name = std::move(name);
                exp.rva = sym.st_value;
                exp.address = base + sym.st_value;

                mod.exports.push_back(std::move(exp));
            }

            // Prefer .dynsym over .symtab for exports (only parse one)
            if (sh.sh_type == SHT_DYNSYM)
            {
                break;
            }
        }
    }

    void parse_section_names(const std::span<const std::byte> data, linux_mapped_module& mod, uint64_t base)
    {
        const auto* ehdr = get_header(data);
        const auto* shdrs = get_section_headers(data);
        if (!shdrs || ehdr->e_shstrndx == 0 || ehdr->e_shstrndx >= ehdr->e_shnum)
        {
            return;
        }

        const auto& shstrtab = shdrs[ehdr->e_shstrndx];

        for (uint16_t i = 0; i < ehdr->e_shnum; ++i)
        {
            const auto& sh = shdrs[i];

            if (!(sh.sh_flags & SHF_ALLOC))
            {
                continue;
            }

            linux_mapped_section sec{};
            sec.name = read_strtab_entry(data, shstrtab.sh_offset, sh.sh_name);
            sec.start = base + sh.sh_addr;
            sec.length = static_cast<size_t>(sh.sh_size);
            sec.permissions =
                elf_segment_to_permission((sh.sh_flags & SHF_EXECINSTR ? PF_X : 0) | (sh.sh_flags & SHF_WRITE ? PF_W : 0) | PF_R);

            mod.sections.push_back(std::move(sec));
        }
    }
}

linux_mapped_module map_elf_from_data(linux_memory_manager& memory, const std::span<const std::byte> data,
                                      const std::filesystem::path& path, const uint64_t forced_base)
{
    if (!is_valid_elf(data) || !is_elf64(data) || !is_little_endian(data))
    {
        throw std::runtime_error("Invalid ELF64 binary: " + path.string());
    }

    const auto* ehdr = get_header(data);
    const auto machine = ehdr->e_machine;

    if (machine != EM_X86_64)
    {
        throw std::runtime_error("Unsupported ELF machine type: " + std::to_string(machine));
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
    {
        throw std::runtime_error("Unsupported ELF type (expected ET_EXEC or ET_DYN): " + std::to_string(ehdr->e_type));
    }

    const auto* phdrs = get_program_headers(data);
    if (!phdrs)
    {
        throw std::runtime_error("No program headers in ELF: " + path.string());
    }

    // Calculate the virtual address range spanned by all PT_LOAD segments
    uint64_t vaddr_min = UINT64_MAX;
    uint64_t vaddr_max = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        const auto& ph = phdrs[i];
        if (ph.p_type != PT_LOAD)
        {
            continue;
        }

        const auto seg_start = page_align_down(ph.p_vaddr);
        const auto seg_end = page_align_up(ph.p_vaddr + ph.p_memsz);

        vaddr_min = std::min(vaddr_min, seg_start);
        vaddr_max = std::max(vaddr_max, seg_end);
    }

    if (vaddr_min >= vaddr_max)
    {
        throw std::runtime_error("No PT_LOAD segments in ELF: " + path.string());
    }

    const auto total_size = static_cast<size_t>(vaddr_max - vaddr_min);

    // For ET_DYN (PIE/shared objects), we need to choose a base address
    uint64_t base = 0;
    if (ehdr->e_type == ET_DYN)
    {
        if (forced_base != 0)
        {
            base = forced_base;
        }
        else
        {
            base = memory.find_free_allocation_base(total_size);
            if (!base)
            {
                throw std::runtime_error("Failed to find free memory for ELF: " + path.string());
            }
        }
    }
    else
    {
        // ET_EXEC: use the vaddr from the binary directly (no relocation)
        base = vaddr_min;
    }

    const int64_t base_delta = static_cast<int64_t>(base) - static_cast<int64_t>(vaddr_min);

    // Map each PT_LOAD segment.
    // Adjacent segments often share a page boundary (e.g., .text ends at 0x405000
    // and .data starts at 0x405ff0, both page-aligning to 0x405000). We must handle
    // this overlap by tracking the highest address mapped so far.
    uint64_t mapped_up_to = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        const auto& ph = phdrs[i];
        if (ph.p_type != PT_LOAD)
        {
            continue;
        }

        auto seg_vaddr = page_align_down(ph.p_vaddr + static_cast<uint64_t>(base_delta));
        const auto seg_end = page_align_up(ph.p_vaddr + ph.p_memsz + static_cast<uint64_t>(base_delta));

        // If this segment's page-aligned start overlaps the previous segment's
        // page-aligned end, skip the already-mapped region
        if (seg_vaddr < mapped_up_to)
        {
            seg_vaddr = mapped_up_to;
        }

        if (seg_vaddr < seg_end)
        {
            const auto seg_size = static_cast<size_t>(seg_end - seg_vaddr);
            const auto perm = elf_segment_to_permission(ph.p_flags);

            // Allocate with read-write initially so we can copy data, then set correct permissions
            if (!memory.allocate_memory(seg_vaddr, seg_size, memory_permission::read_write))
            {
                throw std::runtime_error("Failed to allocate memory for PT_LOAD segment at 0x" + std::to_string(seg_vaddr));
            }

            // Set correct permissions (deferred until after data copy for shared pages)
            if (perm != memory_permission::read_write)
            {
                memory.protect_memory(seg_vaddr, seg_size, perm);
            }
        }

        // Copy file data into the segment
        if (ph.p_filesz > 0 && ph.p_offset + ph.p_filesz <= data.size())
        {
            const auto dest_addr = ph.p_vaddr + static_cast<uint64_t>(base_delta);

            // The shared page between segments may have been mapped read-only by the
            // previous segment. Temporarily make the target range writable for the copy.
            const auto copy_page_start = page_align_down(dest_addr);
            const auto copy_page_end = page_align_up(dest_addr + ph.p_filesz);
            memory.protect_memory(copy_page_start, static_cast<size_t>(copy_page_end - copy_page_start), memory_permission::read_write);

            memory.write_memory(dest_addr, data.data() + ph.p_offset, static_cast<size_t>(ph.p_filesz));

            // Restore correct permissions for the pages we own (the new allocation region)
            const auto perm = elf_segment_to_permission(ph.p_flags);
            if (seg_vaddr < seg_end && perm != memory_permission::read_write)
            {
                memory.protect_memory(seg_vaddr, static_cast<size_t>(seg_end - seg_vaddr), perm);
            }
        }

        // BSS: the area between filesz and memsz is already zero (freshly mapped)

        if (seg_end > mapped_up_to)
        {
            mapped_up_to = seg_end;
        }
    }

    // Build the mapped module info
    linux_mapped_module mod{};
    mod.name = path.filename().string();
    mod.path = path;
    mod.image_base = vaddr_min + static_cast<uint64_t>(base_delta);
    mod.size_of_image = total_size;
    mod.entry_point = ehdr->e_entry + static_cast<uint64_t>(base_delta);
    mod.machine = machine;
    mod.elf_type = ehdr->e_type;

    // PHDR info for auxiliary vector
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type == PT_PHDR)
        {
            mod.phdr_vaddr = phdrs[i].p_vaddr + static_cast<uint64_t>(base_delta);
            break;
        }
    }

    if (mod.phdr_vaddr == 0)
    {
        // If no PT_PHDR, compute from file header
        mod.phdr_vaddr = mod.image_base + ehdr->e_phoff;
    }

    mod.phdr_count = ehdr->e_phnum;
    mod.phdr_entry_size = ehdr->e_phentsize;

    // Parse TLS segment
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type == PT_TLS)
        {
            mod.tls_image_addr = phdrs[i].p_vaddr + static_cast<uint64_t>(base_delta);
            mod.tls_image_size = phdrs[i].p_filesz;
            mod.tls_mem_size = phdrs[i].p_memsz;
            mod.tls_alignment = phdrs[i].p_align ? phdrs[i].p_align : 1;
            break;
        }
    }

    // Parse dynamic section for DT_NEEDED
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type == PT_DYNAMIC)
        {
            parse_dynamic_section(data, phdrs[i], mod, base_delta);
            break;
        }
    }

    // Parse symbols
    parse_symbols(data, mod, static_cast<uint64_t>(base_delta));

    // Parse section names
    parse_section_names(data, mod, static_cast<uint64_t>(base_delta));

    return mod;
}

void apply_elf_relocations(linux_memory_manager& memory, linux_mapped_module& /*mod*/, const std::span<const std::byte> data,
                           const int64_t base_delta)
{
    const auto* ehdr = get_header(data);
    const auto* shdrs = get_section_headers(data);
    if (!shdrs)
    {
        return;
    }

    for (uint16_t i = 0; i < ehdr->e_shnum; ++i)
    {
        const auto& sh = shdrs[i];
        if (sh.sh_type != SHT_RELA)
        {
            continue;
        }

        if (sh.sh_offset + sh.sh_size > data.size())
        {
            continue;
        }

        const auto rela_count = sh.sh_size / sizeof(Elf64_Rela);
        const auto* relas = reinterpret_cast<const Elf64_Rela*>(data.data() + sh.sh_offset);

        // Get the symbol table associated with this relocation section
        const Elf64_Sym* symtab = nullptr;
        size_t symcount = 0;

        if (sh.sh_link < ehdr->e_shnum)
        {
            const auto& symsh = shdrs[sh.sh_link];
            if (symsh.sh_offset + symsh.sh_size <= data.size())
            {
                symtab = reinterpret_cast<const Elf64_Sym*>(data.data() + symsh.sh_offset);
                symcount = symsh.sh_size / sizeof(Elf64_Sym);
            }
        }

        for (size_t j = 0; j < rela_count; ++j)
        {
            const auto& rela = relas[j];
            const auto type = elf64_r_type(rela.r_info);
            const auto sym_idx = elf64_r_sym(rela.r_info);

            const auto target_addr = rela.r_offset + static_cast<uint64_t>(base_delta);

            switch (type)
            {
            case R_X86_64_RELATIVE: {
                // B + A
                const auto value = static_cast<uint64_t>(static_cast<int64_t>(rela.r_addend) + base_delta);
                memory.write_memory(target_addr, &value, sizeof(value));
                break;
            }
            case R_X86_64_64: {
                // S + A
                if (symtab && sym_idx < symcount)
                {
                    const auto sym_value = symtab[sym_idx].st_value + static_cast<uint64_t>(base_delta);
                    const auto value = sym_value + static_cast<uint64_t>(rela.r_addend);
                    memory.write_memory(target_addr, &value, sizeof(value));
                }
                break;
            }
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: {
                // S
                if (symtab && sym_idx < symcount)
                {
                    const auto value = symtab[sym_idx].st_value + static_cast<uint64_t>(base_delta);
                    memory.write_memory(target_addr, &value, sizeof(value));
                }
                break;
            }
            case R_X86_64_COPY:
            case R_X86_64_NONE:
                // COPY is handled during dynamic linking (not for static binaries)
                // NONE is a no-op
                break;

            default:
                // Unhandled relocation type — skip for now
                break;
            }
        }
    }
}
