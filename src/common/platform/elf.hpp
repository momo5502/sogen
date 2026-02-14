#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <optional>

// ============================================================================
// ELF format definitions — portable, no compiler intrinsics
// ============================================================================

namespace elf
{
    // ---- ELF identification ------------------------------------------------

    constexpr uint8_t EI_MAG0 = 0;
    constexpr uint8_t EI_MAG1 = 1;
    constexpr uint8_t EI_MAG2 = 2;
    constexpr uint8_t EI_MAG3 = 3;
    constexpr uint8_t EI_CLASS = 4;
    constexpr uint8_t EI_DATA = 5;
    constexpr uint8_t EI_VERSION = 6;
    constexpr uint8_t EI_OSABI = 7;
    constexpr uint8_t EI_ABIVERSION = 8;
    constexpr uint8_t EI_PAD = 9;
    constexpr uint8_t EI_NIDENT = 16;

    constexpr uint8_t ELFMAG0 = 0x7f;
    constexpr uint8_t ELFMAG1 = 'E';
    constexpr uint8_t ELFMAG2 = 'L';
    constexpr uint8_t ELFMAG3 = 'F';

    constexpr uint8_t ELFCLASSNONE = 0;
    constexpr uint8_t ELFCLASS32 = 1;
    constexpr uint8_t ELFCLASS64 = 2;

    constexpr uint8_t ELFDATANONE = 0;
    constexpr uint8_t ELFDATA2LSB = 1;
    constexpr uint8_t ELFDATA2MSB = 2;

    constexpr uint8_t EV_NONE = 0;
    constexpr uint8_t EV_CURRENT = 1;

    // ---- ELF types ---------------------------------------------------------

    constexpr uint16_t ET_NONE = 0;
    constexpr uint16_t ET_REL = 1;
    constexpr uint16_t ET_EXEC = 2;
    constexpr uint16_t ET_DYN = 3;
    constexpr uint16_t ET_CORE = 4;

    // ---- Machine types -----------------------------------------------------

    constexpr uint16_t EM_NONE = 0;
    constexpr uint16_t EM_386 = 3;
    constexpr uint16_t EM_X86_64 = 62;

    // ---- Program header types (p_type) -------------------------------------

    constexpr uint32_t PT_NULL = 0;
    constexpr uint32_t PT_LOAD = 1;
    constexpr uint32_t PT_DYNAMIC = 2;
    constexpr uint32_t PT_INTERP = 3;
    constexpr uint32_t PT_NOTE = 4;
    constexpr uint32_t PT_SHLIB = 5;
    constexpr uint32_t PT_PHDR = 6;
    constexpr uint32_t PT_TLS = 7;
    constexpr uint32_t PT_GNU_EH_FRAME = 0x6474e550;
    constexpr uint32_t PT_GNU_STACK = 0x6474e551;
    constexpr uint32_t PT_GNU_RELRO = 0x6474e552;

    // ---- Program header flags (p_flags) ------------------------------------

    constexpr uint32_t PF_X = 1;
    constexpr uint32_t PF_W = 2;
    constexpr uint32_t PF_R = 4;

    // ---- Section header types (sh_type) ------------------------------------

    constexpr uint32_t SHT_NULL = 0;
    constexpr uint32_t SHT_PROGBITS = 1;
    constexpr uint32_t SHT_SYMTAB = 2;
    constexpr uint32_t SHT_STRTAB = 3;
    constexpr uint32_t SHT_RELA = 4;
    constexpr uint32_t SHT_HASH = 5;
    constexpr uint32_t SHT_DYNAMIC = 6;
    constexpr uint32_t SHT_NOTE = 7;
    constexpr uint32_t SHT_NOBITS = 8;
    constexpr uint32_t SHT_REL = 9;
    constexpr uint32_t SHT_DYNSYM = 11;

    // ---- Section header flags (sh_flags) -----------------------------------

    constexpr uint64_t SHF_WRITE = 0x1;
    constexpr uint64_t SHF_ALLOC = 0x2;
    constexpr uint64_t SHF_EXECINSTR = 0x4;

    // ---- Dynamic section tags (d_tag) --------------------------------------

    constexpr int64_t DT_NULL = 0;
    constexpr int64_t DT_NEEDED = 1;
    constexpr int64_t DT_PLTRELSZ = 2;
    constexpr int64_t DT_PLTGOT = 3;
    constexpr int64_t DT_HASH = 4;
    constexpr int64_t DT_STRTAB = 5;
    constexpr int64_t DT_SYMTAB = 6;
    constexpr int64_t DT_RELA = 7;
    constexpr int64_t DT_RELASZ = 8;
    constexpr int64_t DT_RELAENT = 9;
    constexpr int64_t DT_STRSZ = 10;
    constexpr int64_t DT_SYMENT = 11;
    constexpr int64_t DT_INIT = 12;
    constexpr int64_t DT_FINI = 13;
    constexpr int64_t DT_SONAME = 14;
    constexpr int64_t DT_RPATH = 15;
    constexpr int64_t DT_SYMBOLIC = 16;
    constexpr int64_t DT_REL = 17;
    constexpr int64_t DT_RELSZ = 18;
    constexpr int64_t DT_RELENT = 19;
    constexpr int64_t DT_PLTREL = 20;
    constexpr int64_t DT_DEBUG = 21;
    constexpr int64_t DT_TEXTREL = 22;
    constexpr int64_t DT_JMPREL = 23;
    constexpr int64_t DT_BIND_NOW = 24;
    constexpr int64_t DT_INIT_ARRAY = 25;
    constexpr int64_t DT_FINI_ARRAY = 26;
    constexpr int64_t DT_INIT_ARRAYSZ = 27;
    constexpr int64_t DT_FINI_ARRAYSZ = 28;
    constexpr int64_t DT_RUNPATH = 29;
    constexpr int64_t DT_FLAGS = 30;
    constexpr int64_t DT_FLAGS_1 = 0x6ffffffb;
    constexpr int64_t DT_VERNEED = 0x6ffffffe;
    constexpr int64_t DT_VERNEEDNUM = 0x6fffffff;
    constexpr int64_t DT_VERSYM = 0x6ffffff0;
    constexpr int64_t DT_RELACOUNT = 0x6ffffff9;

    // ---- Symbol binding and type macros ------------------------------------

    constexpr uint8_t STB_LOCAL = 0;
    constexpr uint8_t STB_GLOBAL = 1;
    constexpr uint8_t STB_WEAK = 2;

    constexpr uint8_t STT_NOTYPE = 0;
    constexpr uint8_t STT_OBJECT = 1;
    constexpr uint8_t STT_FUNC = 2;
    constexpr uint8_t STT_SECTION = 3;
    constexpr uint8_t STT_FILE = 4;
    constexpr uint8_t STT_TLS = 6;

    constexpr uint16_t SHN_UNDEF = 0;
    constexpr uint16_t SHN_ABS = 0xFFF1;
    constexpr uint16_t SHN_COMMON = 0xFFF2;

    inline constexpr uint8_t elf64_st_bind(uint8_t info)
    {
        return info >> 4;
    }

    inline constexpr uint8_t elf64_st_type(uint8_t info)
    {
        return info & 0x0F;
    }

    inline constexpr uint8_t elf64_st_info(uint8_t bind, uint8_t type)
    {
        return static_cast<uint8_t>((bind << 4) + (type & 0x0F));
    }

    // ---- x86-64 Relocation types -------------------------------------------

    constexpr uint32_t R_X86_64_NONE = 0;
    constexpr uint32_t R_X86_64_64 = 1;
    constexpr uint32_t R_X86_64_PC32 = 2;
    constexpr uint32_t R_X86_64_GOT32 = 3;
    constexpr uint32_t R_X86_64_PLT32 = 4;
    constexpr uint32_t R_X86_64_COPY = 5;
    constexpr uint32_t R_X86_64_GLOB_DAT = 6;
    constexpr uint32_t R_X86_64_JUMP_SLOT = 7;
    constexpr uint32_t R_X86_64_RELATIVE = 8;
    constexpr uint32_t R_X86_64_GOTPCREL = 9;
    constexpr uint32_t R_X86_64_32 = 10;
    constexpr uint32_t R_X86_64_32S = 11;
    constexpr uint32_t R_X86_64_DTPMOD64 = 16;
    constexpr uint32_t R_X86_64_DTPOFF64 = 17;
    constexpr uint32_t R_X86_64_TPOFF64 = 18;
    constexpr uint32_t R_X86_64_IRELATIVE = 37;

    inline constexpr uint32_t elf64_r_sym(uint64_t info)
    {
        return static_cast<uint32_t>(info >> 32);
    }

    inline constexpr uint32_t elf64_r_type(uint64_t info)
    {
        return static_cast<uint32_t>(info & 0xFFFFFFFF);
    }

    inline constexpr uint64_t elf64_r_info(uint32_t sym, uint32_t type)
    {
        return (static_cast<uint64_t>(sym) << 32) | type;
    }

    // ---- Auxiliary vector types (for initial stack) -------------------------

    constexpr uint64_t AT_NULL = 0;
    constexpr uint64_t AT_IGNORE = 1;
    constexpr uint64_t AT_EXECFD = 2;
    constexpr uint64_t AT_PHDR = 3;
    constexpr uint64_t AT_PHENT = 4;
    constexpr uint64_t AT_PHNUM = 5;
    constexpr uint64_t AT_PAGESZ = 6;
    constexpr uint64_t AT_BASE = 7;
    constexpr uint64_t AT_FLAGS = 8;
    constexpr uint64_t AT_ENTRY = 9;
    constexpr uint64_t AT_NOTELF = 10;
    constexpr uint64_t AT_UID = 11;
    constexpr uint64_t AT_EUID = 12;
    constexpr uint64_t AT_GID = 13;
    constexpr uint64_t AT_EGID = 14;
    constexpr uint64_t AT_PLATFORM = 15;
    constexpr uint64_t AT_HWCAP = 16;
    constexpr uint64_t AT_CLKTCK = 17;
    constexpr uint64_t AT_SECURE = 23;
    constexpr uint64_t AT_BASE_PLATFORM = 24;
    constexpr uint64_t AT_RANDOM = 25;
    constexpr uint64_t AT_HWCAP2 = 26;
    constexpr uint64_t AT_EXECFN = 31;
    constexpr uint64_t AT_SYSINFO_EHDR = 33;

    // ========================================================================
    // Structure definitions
    // ========================================================================

#pragma pack(push, 1)

    struct Elf64_Ehdr
    {
        std::array<uint8_t, EI_NIDENT> e_ident;
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint64_t e_entry;
        uint64_t e_phoff;
        uint64_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    };

    static_assert(sizeof(Elf64_Ehdr) == 64);

    struct Elf64_Phdr
    {
        uint32_t p_type;
        uint32_t p_flags;
        uint64_t p_offset;
        uint64_t p_vaddr;
        uint64_t p_paddr;
        uint64_t p_filesz;
        uint64_t p_memsz;
        uint64_t p_align;
    };

    static_assert(sizeof(Elf64_Phdr) == 56);

    struct Elf64_Shdr
    {
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_flags;
        uint64_t sh_addr;
        uint64_t sh_offset;
        uint64_t sh_size;
        uint32_t sh_link;
        uint32_t sh_info;
        uint64_t sh_addralign;
        uint64_t sh_entsize;
    };

    static_assert(sizeof(Elf64_Shdr) == 64);

    struct Elf64_Sym
    {
        uint32_t st_name;
        uint8_t st_info;
        uint8_t st_other;
        uint16_t st_shndx;
        uint64_t st_value;
        uint64_t st_size;
    };

    static_assert(sizeof(Elf64_Sym) == 24);

    struct Elf64_Rel
    {
        uint64_t r_offset;
        uint64_t r_info;
    };

    static_assert(sizeof(Elf64_Rel) == 16);

    struct Elf64_Rela
    {
        uint64_t r_offset;
        uint64_t r_info;
        int64_t r_addend;
    };

    static_assert(sizeof(Elf64_Rela) == 24);

    struct Elf64_Dyn
    {
        int64_t d_tag;
        union
        {
            uint64_t d_val;
            uint64_t d_ptr;
        } d_un;
    };

    static_assert(sizeof(Elf64_Dyn) == 16);

    struct Elf64_auxv_t
    {
        uint64_t a_type;
        uint64_t a_val;
    };

    static_assert(sizeof(Elf64_auxv_t) == 16);

#pragma pack(pop)

    // ========================================================================
    // Utility functions
    // ========================================================================

    inline bool is_valid_elf(const std::span<const std::byte> data)
    {
        if (data.size() < sizeof(Elf64_Ehdr))
        {
            return false;
        }

        const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());
        return ehdr->e_ident[EI_MAG0] == ELFMAG0 && ehdr->e_ident[EI_MAG1] == ELFMAG1 && ehdr->e_ident[EI_MAG2] == ELFMAG2 &&
               ehdr->e_ident[EI_MAG3] == ELFMAG3;
    }

    inline bool is_elf64(const std::span<const std::byte> data)
    {
        if (!is_valid_elf(data))
        {
            return false;
        }

        const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());
        return ehdr->e_ident[EI_CLASS] == ELFCLASS64;
    }

    inline bool is_little_endian(const std::span<const std::byte> data)
    {
        if (!is_valid_elf(data))
        {
            return false;
        }

        const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());
        return ehdr->e_ident[EI_DATA] == ELFDATA2LSB;
    }

    inline std::optional<uint16_t> get_elf_machine(const std::span<const std::byte> data)
    {
        if (!is_valid_elf(data))
        {
            return std::nullopt;
        }

        const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());
        return ehdr->e_machine;
    }

    inline const Elf64_Ehdr* get_header(const std::span<const std::byte> data)
    {
        if (data.size() < sizeof(Elf64_Ehdr))
        {
            return nullptr;
        }

        return reinterpret_cast<const Elf64_Ehdr*>(data.data());
    }

    inline const Elf64_Phdr* get_program_headers(const std::span<const std::byte> data)
    {
        const auto* ehdr = get_header(data);
        if (!ehdr || !ehdr->e_phoff || !ehdr->e_phnum)
        {
            return nullptr;
        }

        if (data.size() < ehdr->e_phoff + static_cast<uint64_t>(ehdr->e_phnum) * ehdr->e_phentsize)
        {
            return nullptr;
        }

        return reinterpret_cast<const Elf64_Phdr*>(data.data() + ehdr->e_phoff);
    }

    inline const Elf64_Shdr* get_section_headers(const std::span<const std::byte> data)
    {
        const auto* ehdr = get_header(data);
        if (!ehdr || !ehdr->e_shoff || !ehdr->e_shnum)
        {
            return nullptr;
        }

        if (data.size() < ehdr->e_shoff + static_cast<uint64_t>(ehdr->e_shnum) * ehdr->e_shentsize)
        {
            return nullptr;
        }

        return reinterpret_cast<const Elf64_Shdr*>(data.data() + ehdr->e_shoff);
    }

} // namespace elf
