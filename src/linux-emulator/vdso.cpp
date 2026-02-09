#include "std_include.hpp"
#include "vdso.hpp"

#include <platform/elf.hpp>

#include <cstring>

using namespace elf;

// ============================================================================
// Synthetic vDSO builder
//
// We construct a minimal valid ELF64 shared object entirely in emulator memory.
// The layout is:
//
//   Offset   Content
//   0x000    ELF header (64 bytes)
//   0x040    Program headers (2 * 56 = 112 bytes): PT_LOAD, PT_DYNAMIC
//   0x0B0    .text section: 4 syscall stubs (7 bytes each = 28 bytes)
//   0x0D0    .dynstr section: string table
//   0x???    .dynsym section: 9 symbol entries (null + 4 vdso + 4 aliases)
//   0x???    .hash section: ELF hash table
//   0x???    .dynamic section: dynamic entries
//   0x???    Section headers (optional — not strictly required for runtime)
//
// Each function stub is:
//   mov eax, <syscall_nr>   ; B8 xx xx xx xx     (5 bytes)
//   syscall                 ; 0F 05               (2 bytes)
//   ret                     ; C3                   (1 byte)
//   Total: 8 bytes per stub
//
// ============================================================================

namespace
{
    // Syscall numbers for vDSO functions
    constexpr uint32_t SYS_GETTIMEOFDAY = 96;
    constexpr uint32_t SYS_TIME = 201;
    constexpr uint32_t SYS_CLOCK_GETTIME = 228;
    constexpr uint32_t SYS_GETCPU = 309;

    // vDSO is mapped at a fixed address in the higher user-space region.
    // Real Linux randomizes this, but we use a fixed address for simplicity.
    constexpr uint64_t VDSO_BASE = 0x7ffff7ff0000ULL;

    // Function stub: mov eax, <nr>; syscall; ret
    constexpr size_t STUB_SIZE = 8;

    void write_stub(std::vector<uint8_t>& image, const size_t offset, const uint32_t syscall_nr)
    {
        // B8 xx xx xx xx   mov eax, imm32
        image[offset + 0] = 0xB8;
        image[offset + 1] = static_cast<uint8_t>(syscall_nr & 0xFF);
        image[offset + 2] = static_cast<uint8_t>((syscall_nr >> 8) & 0xFF);
        image[offset + 3] = static_cast<uint8_t>((syscall_nr >> 16) & 0xFF);
        image[offset + 4] = static_cast<uint8_t>((syscall_nr >> 24) & 0xFF);
        // 0F 05   syscall
        image[offset + 5] = 0x0F;
        image[offset + 6] = 0x05;
        // C3   ret
        image[offset + 7] = 0xC3;
    }

    // Simple ELF hash function (per the ELF spec)
    uint32_t elf_hash(const char* name)
    {
        uint32_t h = 0;
        while (*name)
        {
            h = (h << 4) + static_cast<uint8_t>(*name++);
            const auto g = h & 0xF0000000;
            if (g)
            {
                h ^= g >> 24;
            }
            h &= ~g;
        }
        return h;
    }

    // Helper to write a uint32_t at a byte offset
    void put_u32(std::vector<uint8_t>& img, const size_t off, const uint32_t val)
    {
        memcpy(&img[off], &val, sizeof(val));
    }

    // Helper to write a struct at a byte offset
    template <typename T>
    void put_struct(std::vector<uint8_t>& img, const size_t off, const T& val)
    {
        memcpy(&img[off], &val, sizeof(val));
    }
}

uint64_t linux_vdso::setup(linux_memory_manager& memory)
{
    // ---- Define symbol names ----
    // We export both __vdso_xxx and bare xxx names (aliases).
    // glibc looks for __vdso_clock_gettime, musl looks for clock_gettime.
    struct vdso_func
    {
        const char* vdso_name;  // e.g. "__vdso_clock_gettime"
        const char* alias_name; // e.g. "clock_gettime"
        uint32_t syscall_nr;
    };

    const vdso_func funcs[] = {
        {"__vdso_clock_gettime", "clock_gettime", SYS_CLOCK_GETTIME},
        {"__vdso_gettimeofday", "gettimeofday", SYS_GETTIMEOFDAY},
        {"__vdso_time", "time", SYS_TIME},
        {"__vdso_getcpu", "getcpu", SYS_GETCPU},
    };

    constexpr size_t NUM_FUNCS = 4;

    // ---- Plan layout ----
    //
    // We lay everything out in a single page (0x1000 bytes).
    // All offsets are relative to VDSO_BASE.

    constexpr size_t VDSO_PAGE_SIZE = 0x1000;

    // ELF header: 0x000 - 0x03F (64 bytes)
    constexpr size_t EHDR_OFF = 0;

    // Program headers: 0x040 (2 entries * 56 = 112 bytes)
    constexpr size_t PHDR_OFF = sizeof(Elf64_Ehdr); // 0x40
    constexpr size_t NUM_PHDRS = 2;
    constexpr size_t PHDR_SIZE = NUM_PHDRS * sizeof(Elf64_Phdr); // 112

    // .text: starts after phdrs, aligned to 16
    const size_t text_off = (PHDR_OFF + PHDR_SIZE + 15) & ~size_t{15}; // 0x0B0
    const size_t text_size = NUM_FUNCS * STUB_SIZE;                    // 32

    // .dynstr: after .text, aligned to 8
    const size_t dynstr_off = (text_off + text_size + 7) & ~size_t{7};

    // Build dynstr content first to know its size
    // Format: \0 + all symbol name strings
    std::string dynstr;
    dynstr += '\0'; // index 0 = empty string

    // Record string table indices for each name
    uint32_t vdso_name_idx[NUM_FUNCS]{};
    uint32_t alias_name_idx[NUM_FUNCS]{};
    uint32_t soname_idx{};

    // Add soname
    soname_idx = static_cast<uint32_t>(dynstr.size());
    dynstr += "linux-vdso.so.1";
    dynstr += '\0';

    for (size_t i = 0; i < NUM_FUNCS; ++i)
    {
        vdso_name_idx[i] = static_cast<uint32_t>(dynstr.size());
        dynstr += funcs[i].vdso_name;
        dynstr += '\0';

        alias_name_idx[i] = static_cast<uint32_t>(dynstr.size());
        dynstr += funcs[i].alias_name;
        dynstr += '\0';
    }

    const size_t dynstr_size = dynstr.size();

    // .dynsym: after .dynstr, aligned to 8
    // Entries: [0]=null, [1..4]=__vdso_xxx, [5..8]=alias xxx
    constexpr size_t NUM_SYMS = 1 + NUM_FUNCS * 2; // 9
    const size_t dynsym_off = (dynstr_off + dynstr_size + 7) & ~size_t{7};
    constexpr size_t dynsym_size = NUM_SYMS * sizeof(Elf64_Sym);

    // .hash: after .dynsym, aligned to 4
    // Simple ELF hash table: nbucket, nchain, bucket[nbucket], chain[nchain]
    constexpr uint32_t HASH_NBUCKET = 7; // small prime > NUM_SYMS
    constexpr uint32_t HASH_NCHAIN = NUM_SYMS;
    constexpr size_t hash_header_size = 2 * sizeof(uint32_t);
    constexpr size_t hash_data_size = (HASH_NBUCKET + HASH_NCHAIN) * sizeof(uint32_t);
    const size_t hash_off = (dynsym_off + dynsym_size + 3) & ~size_t{3};
    constexpr size_t hash_size = hash_header_size + hash_data_size;

    // .dynamic: after .hash, aligned to 8
    const size_t dynamic_off = (hash_off + hash_size + 7) & ~size_t{7};
    // Entries: DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT, DT_SONAME, DT_NULL
    constexpr size_t NUM_DYN_ENTRIES = 7;
    constexpr size_t dynamic_size = NUM_DYN_ENTRIES * sizeof(Elf64_Dyn);

    // Section headers: after .dynamic, aligned to 8 (optional but nice for tools)
    const size_t shdr_off = (dynamic_off + dynamic_size + 7) & ~size_t{7};
    // Sections: [0]=null, [1]=.text, [2]=.dynstr, [3]=.dynsym, [4]=.hash, [5]=.dynamic, [6]=.shstrtab
    constexpr size_t NUM_SECTIONS = 7;
    constexpr size_t shdr_size = NUM_SECTIONS * sizeof(Elf64_Shdr);

    // .shstrtab: after section headers
    const size_t shstrtab_off = shdr_off + shdr_size;
    // Build section name string table
    std::string shstrtab;
    shstrtab += '\0'; // index 0
    const auto shname_text = static_cast<uint32_t>(shstrtab.size());
    shstrtab += ".text";
    shstrtab += '\0';
    const auto shname_dynstr = static_cast<uint32_t>(shstrtab.size());
    shstrtab += ".dynstr";
    shstrtab += '\0';
    const auto shname_dynsym = static_cast<uint32_t>(shstrtab.size());
    shstrtab += ".dynsym";
    shstrtab += '\0';
    const auto shname_hash = static_cast<uint32_t>(shstrtab.size());
    shstrtab += ".hash";
    shstrtab += '\0';
    const auto shname_dynamic = static_cast<uint32_t>(shstrtab.size());
    shstrtab += ".dynamic";
    shstrtab += '\0';
    const auto shname_shstrtab = static_cast<uint32_t>(shstrtab.size());
    shstrtab += ".shstrtab";
    shstrtab += '\0';

    const size_t shstrtab_size = shstrtab.size();
    const size_t total_size = shstrtab_off + shstrtab_size;

    // Ensure it fits in one page
    if (total_size > VDSO_PAGE_SIZE)
    {
        // Should not happen with our small vDSO
        return 0;
    }

    // ---- Build the image ----
    std::vector<uint8_t> image(VDSO_PAGE_SIZE, 0);

    // -- ELF header --
    Elf64_Ehdr ehdr{};
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = 0; // ELFOSABI_NONE
    ehdr.e_type = ET_DYN;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = 0;
    ehdr.e_phoff = PHDR_OFF;
    ehdr.e_shoff = shdr_off;
    ehdr.e_flags = 0;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = NUM_PHDRS;
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum = NUM_SECTIONS;
    ehdr.e_shstrndx = NUM_SECTIONS - 1; // .shstrtab is last
    put_struct(image, EHDR_OFF, ehdr);

    // -- Program headers --
    // PT_LOAD: map entire page as r-x
    {
        Elf64_Phdr phdr{};
        phdr.p_type = PT_LOAD;
        phdr.p_flags = PF_R | PF_X;
        phdr.p_offset = 0;
        phdr.p_vaddr = 0; // Relative to base (ET_DYN)
        phdr.p_paddr = 0;
        phdr.p_filesz = VDSO_PAGE_SIZE;
        phdr.p_memsz = VDSO_PAGE_SIZE;
        phdr.p_align = VDSO_PAGE_SIZE;
        put_struct(image, PHDR_OFF, phdr);
    }

    // PT_DYNAMIC: points to .dynamic section
    {
        Elf64_Phdr phdr{};
        phdr.p_type = PT_DYNAMIC;
        phdr.p_flags = PF_R;
        phdr.p_offset = dynamic_off;
        phdr.p_vaddr = dynamic_off;
        phdr.p_paddr = dynamic_off;
        phdr.p_filesz = dynamic_size;
        phdr.p_memsz = dynamic_size;
        phdr.p_align = 8;
        put_struct(image, PHDR_OFF + sizeof(Elf64_Phdr), phdr);
    }

    // -- .text: syscall stubs --
    for (size_t i = 0; i < NUM_FUNCS; ++i)
    {
        write_stub(image, text_off + i * STUB_SIZE, funcs[i].syscall_nr);
    }

    // -- .dynstr --
    memcpy(&image[dynstr_off], dynstr.data(), dynstr_size);

    // -- .dynsym --
    // Entry [0]: null symbol (already zero-filled)

    for (size_t i = 0; i < NUM_FUNCS; ++i)
    {
        // __vdso_xxx symbol
        Elf64_Sym sym{};
        sym.st_name = vdso_name_idx[i];
        sym.st_info = elf64_st_info(STB_GLOBAL, STT_FUNC);
        sym.st_other = 0;
        sym.st_shndx = 1; // .text section index
        sym.st_value = text_off + i * STUB_SIZE;
        sym.st_size = STUB_SIZE;
        put_struct(image, dynsym_off + (1 + i) * sizeof(Elf64_Sym), sym);

        // alias symbol (same function)
        Elf64_Sym alias{};
        alias.st_name = alias_name_idx[i];
        alias.st_info = elf64_st_info(STB_WEAK, STT_FUNC);
        alias.st_other = 0;
        alias.st_shndx = 1;
        alias.st_value = text_off + i * STUB_SIZE;
        alias.st_size = STUB_SIZE;
        put_struct(image, dynsym_off + (1 + NUM_FUNCS + i) * sizeof(Elf64_Sym), alias);
    }

    // -- .hash --
    // ELF hash table format: nbucket, nchain, bucket[nbucket], chain[nchain]
    put_u32(image, hash_off + 0, HASH_NBUCKET);
    put_u32(image, hash_off + 4, HASH_NCHAIN);

    const size_t bucket_off = hash_off + 8;
    const size_t chain_off = bucket_off + HASH_NBUCKET * 4;

    // Initialize buckets to 0 (STN_UNDEF = end of chain)
    // Initialize chains to 0

    // Insert each symbol into the hash table
    for (uint32_t sym_idx = 1; sym_idx < NUM_SYMS; ++sym_idx)
    {
        // Read the symbol's name index
        Elf64_Sym sym{};
        memcpy(&sym, &image[dynsym_off + sym_idx * sizeof(Elf64_Sym)], sizeof(sym));

        const char* name = reinterpret_cast<const char*>(&image[dynstr_off + sym.st_name]);
        const auto h = elf_hash(name) % HASH_NBUCKET;

        // Read current bucket value
        uint32_t current{};
        memcpy(&current, &image[bucket_off + h * 4], 4);

        if (current == 0)
        {
            // Bucket is empty — point directly to this symbol
            put_u32(image, bucket_off + h * 4, sym_idx);
        }
        else
        {
            // Walk the chain to find the end
            uint32_t prev = current;
            uint32_t next{};
            memcpy(&next, &image[chain_off + prev * 4], 4);
            while (next != 0)
            {
                prev = next;
                memcpy(&next, &image[chain_off + prev * 4], 4);
            }
            // Append to end of chain
            put_u32(image, chain_off + prev * 4, sym_idx);
        }
    }

    // -- .dynamic --
    size_t dyn_write = dynamic_off;
    auto write_dyn = [&](const int64_t tag, const uint64_t val) {
        Elf64_Dyn dyn{};
        dyn.d_tag = tag;
        dyn.d_un.d_val = val;
        put_struct(image, dyn_write, dyn);
        dyn_write += sizeof(Elf64_Dyn);
    };

    write_dyn(DT_HASH, hash_off);     // Address of .hash (relative to base for ET_DYN)
    write_dyn(DT_STRTAB, dynstr_off); // Address of .dynstr
    write_dyn(DT_SYMTAB, dynsym_off); // Address of .dynsym
    write_dyn(DT_STRSZ, static_cast<uint64_t>(dynstr_size));
    write_dyn(DT_SYMENT, sizeof(Elf64_Sym));
    write_dyn(DT_SONAME, soname_idx);
    write_dyn(DT_NULL, 0);

    // -- Section headers --
    // [0] = null
    // (already zero-filled)

    // [1] = .text
    {
        Elf64_Shdr shdr{};
        shdr.sh_name = shname_text;
        shdr.sh_type = SHT_PROGBITS;
        shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
        shdr.sh_addr = text_off;
        shdr.sh_offset = text_off;
        shdr.sh_size = text_size;
        shdr.sh_addralign = 16;
        put_struct(image, shdr_off + 1 * sizeof(Elf64_Shdr), shdr);
    }

    // [2] = .dynstr
    {
        Elf64_Shdr shdr{};
        shdr.sh_name = shname_dynstr;
        shdr.sh_type = SHT_STRTAB;
        shdr.sh_flags = SHF_ALLOC;
        shdr.sh_addr = dynstr_off;
        shdr.sh_offset = dynstr_off;
        shdr.sh_size = dynstr_size;
        shdr.sh_addralign = 1;
        put_struct(image, shdr_off + 2 * sizeof(Elf64_Shdr), shdr);
    }

    // [3] = .dynsym
    {
        Elf64_Shdr shdr{};
        shdr.sh_name = shname_dynsym;
        shdr.sh_type = SHT_DYNSYM;
        shdr.sh_flags = SHF_ALLOC;
        shdr.sh_addr = dynsym_off;
        shdr.sh_offset = dynsym_off;
        shdr.sh_size = dynsym_size;
        shdr.sh_link = 2; // .dynstr
        shdr.sh_info = 1; // First non-local symbol
        shdr.sh_addralign = 8;
        shdr.sh_entsize = sizeof(Elf64_Sym);
        put_struct(image, shdr_off + 3 * sizeof(Elf64_Shdr), shdr);
    }

    // [4] = .hash
    {
        Elf64_Shdr shdr{};
        shdr.sh_name = shname_hash;
        shdr.sh_type = SHT_HASH;
        shdr.sh_flags = SHF_ALLOC;
        shdr.sh_addr = hash_off;
        shdr.sh_offset = hash_off;
        shdr.sh_size = hash_size;
        shdr.sh_link = 3; // .dynsym
        shdr.sh_addralign = 4;
        shdr.sh_entsize = 4;
        put_struct(image, shdr_off + 4 * sizeof(Elf64_Shdr), shdr);
    }

    // [5] = .dynamic
    {
        Elf64_Shdr shdr{};
        shdr.sh_name = shname_dynamic;
        shdr.sh_type = SHT_DYNAMIC;
        shdr.sh_flags = SHF_ALLOC;
        shdr.sh_addr = dynamic_off;
        shdr.sh_offset = dynamic_off;
        shdr.sh_size = dynamic_size;
        shdr.sh_link = 2; // .dynstr
        shdr.sh_addralign = 8;
        shdr.sh_entsize = sizeof(Elf64_Dyn);
        put_struct(image, shdr_off + 5 * sizeof(Elf64_Shdr), shdr);
    }

    // [6] = .shstrtab
    {
        Elf64_Shdr shdr{};
        shdr.sh_name = shname_shstrtab;
        shdr.sh_type = SHT_STRTAB;
        shdr.sh_flags = 0;
        shdr.sh_addr = 0;
        shdr.sh_offset = shstrtab_off;
        shdr.sh_size = shstrtab_size;
        shdr.sh_addralign = 1;
        put_struct(image, shdr_off + 6 * sizeof(Elf64_Shdr), shdr);
    }

    // -- .shstrtab data --
    memcpy(&image[shstrtab_off], shstrtab.data(), shstrtab_size);

    // ---- Map into emulated memory ----
    const auto base = VDSO_BASE;

    if (!memory.allocate_memory(base, VDSO_PAGE_SIZE, memory_permission::read | memory_permission::exec))
    {
        return 0;
    }

    // Write the image
    memory.write_memory(base, image.data(), VDSO_PAGE_SIZE);

    this->base_address_ = base;
    this->image_size_ = VDSO_PAGE_SIZE;

    return base;
}
