#pragma once

#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <variant>

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

#define IMAGE_DIRECTORY_ENTRY_EXPORT          0 // Export Directory
#define IMAGE_DIRECTORY_ENTRY_IMPORT          1 // Import Directory
#define IMAGE_DIRECTORY_ENTRY_RESOURCE        2 // Resource Directory
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION       3 // Exception Directory
#define IMAGE_DIRECTORY_ENTRY_SECURITY        4 // Security Directory
#define IMAGE_DIRECTORY_ENTRY_BASERELOC       5 // Base Relocation Table
#define IMAGE_DIRECTORY_ENTRY_DEBUG           6 // Debug Directory
//      IMAGE_DIRECTORY_ENTRY_COPYRIGHT       7   // (X86 usage)
#define IMAGE_DIRECTORY_ENTRY_ARCHITECTURE    7  // Architecture Specific Data
#define IMAGE_DIRECTORY_ENTRY_GLOBALPTR       8  // RVA of GP
#define IMAGE_DIRECTORY_ENTRY_TLS             9  // TLS Directory
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG     10 // Load Configuration Directory
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT    11 // Bound Import Directory in headers
#define IMAGE_DIRECTORY_ENTRY_IAT             12 // Import Address Table
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT    13 // Delay Load Import Descriptors
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR  14 // COM Runtime descriptor

#define IMAGE_SCN_LNK_NRELOC_OVFL             0x01000000 // Section contains extended relocations.
#define IMAGE_SCN_MEM_DISCARDABLE             0x02000000 // Section can be discarded.
#define IMAGE_SCN_MEM_NOT_CACHED              0x04000000 // Section is not cachable.
#define IMAGE_SCN_MEM_NOT_PAGED               0x08000000 // Section is not pageable.
#define IMAGE_SCN_MEM_SHARED                  0x10000000 // Section is shareable.
#define IMAGE_SCN_MEM_EXECUTE                 0x20000000 // Section is executable.
#define IMAGE_SCN_MEM_READ                    0x40000000 // Section is readable.
#define IMAGE_SCN_MEM_WRITE                   0x80000000 // Section is writeable.

#define IMAGE_SCN_CNT_CODE                    0x00000020 // Section contains code.
#define IMAGE_SCN_CNT_INITIALIZED_DATA        0x00000040 // Section contains initialized data.
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA      0x00000080 // Section contains uninitialized data.

#define IMAGE_REL_BASED_ABSOLUTE              0
#define IMAGE_REL_BASED_HIGH                  1
#define IMAGE_REL_BASED_LOW                   2
#define IMAGE_REL_BASED_HIGHLOW               3
#define IMAGE_REL_BASED_HIGHADJ               4
#define IMAGE_REL_BASED_MIPS_JMPADDR          5
#define IMAGE_REL_BASED_ARM_MOV32A            5
#define IMAGE_REL_BASED_ARM_MOV32             5
#define IMAGE_REL_BASED_SECTION               6
#define IMAGE_REL_BASED_REL                   7
#define IMAGE_REL_BASED_ARM_MOV32T            7
#define IMAGE_REL_BASED_THUMB_MOV32           7
#define IMAGE_REL_BASED_MIPS_JMPADDR16        9
#define IMAGE_REL_BASED_IA64_IMM64            9
#define IMAGE_REL_BASED_DIR64                 10
#define IMAGE_REL_BASED_HIGH3ADJ              11

#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE 0x0040
#define IMAGE_FILE_DLL                        0x2000

#ifndef OS_WINDOWS
#define IMAGE_FILE_MACHINE_UNKNOWN     0
#define IMAGE_FILE_MACHINE_TARGET_HOST 0x0001 // Useful for indicating we want to interact with the host and not a WoW guest.
#define IMAGE_FILE_MACHINE_I386        0x014c // Intel 386.
#define IMAGE_FILE_MACHINE_R3000       0x0162 // MIPS little-endian, 0x160 big-endian
#define IMAGE_FILE_MACHINE_R4000       0x0166 // MIPS little-endian
#define IMAGE_FILE_MACHINE_R10000      0x0168 // MIPS little-endian
#define IMAGE_FILE_MACHINE_WCEMIPSV2   0x0169 // MIPS little-endian WCE v2
#define IMAGE_FILE_MACHINE_ALPHA       0x0184 // Alpha_AXP
#define IMAGE_FILE_MACHINE_SH3         0x01a2 // SH3 little-endian
#define IMAGE_FILE_MACHINE_SH3DSP      0x01a3
#define IMAGE_FILE_MACHINE_SH3E        0x01a4 // SH3E little-endian
#define IMAGE_FILE_MACHINE_SH4         0x01a6 // SH4 little-endian
#define IMAGE_FILE_MACHINE_SH5         0x01a8 // SH5
#define IMAGE_FILE_MACHINE_ARM         0x01c0 // ARM Little-Endian
#define IMAGE_FILE_MACHINE_THUMB       0x01c2 // ARM Thumb/Thumb-2 Little-Endian
#define IMAGE_FILE_MACHINE_ARMNT       0x01c4 // ARM Thumb-2 Little-Endian
#define IMAGE_FILE_MACHINE_AM33        0x01d3
#define IMAGE_FILE_MACHINE_POWERPC     0x01F0 // IBM PowerPC Little-Endian
#define IMAGE_FILE_MACHINE_POWERPCFP   0x01f1
#define IMAGE_FILE_MACHINE_IA64        0x0200 // Intel 64
#define IMAGE_FILE_MACHINE_MIPS16      0x0266 // MIPS
#define IMAGE_FILE_MACHINE_ALPHA64     0x0284 // ALPHA64
#define IMAGE_FILE_MACHINE_MIPSFPU     0x0366 // MIPS
#define IMAGE_FILE_MACHINE_MIPSFPU16   0x0466 // MIPS
#define IMAGE_FILE_MACHINE_AXP64       IMAGE_FILE_MACHINE_ALPHA64
#define IMAGE_FILE_MACHINE_TRICORE     0x0520 // Infineon
#define IMAGE_FILE_MACHINE_CEF         0x0CEF
#define IMAGE_FILE_MACHINE_EBC         0x0EBC // EFI Byte Code
#define IMAGE_FILE_MACHINE_AMD64       0x8664 // AMD64 (K8)
#define IMAGE_FILE_MACHINE_M32R        0x9041 // M32R little-endian
#define IMAGE_FILE_MACHINE_ARM64       0xAA64 // ARM64 Little-Endian
#define IMAGE_FILE_MACHINE_CEE         0xC0EE
#endif

#define PROCESSOR_ARCHITECTURE_AMD64 9

enum class PEMachineType : std::uint16_t
{
    UNKNOWN = 0,
    I386 = 0x014c,      // Intel 386.
    R3000 = 0x0162,     // MIPS little-endian, 0x160 big-endian
    R4000 = 0x0166,     // MIPS little-endian
    R10000 = 0x0168,    // MIPS little-endian
    WCEMIPSV2 = 0x0169, // MIPS little-endian WCE v2
    ALPHA = 0x0184,     // Alpha_AXP
    SH3 = 0x01a2,       // SH3 little-endian
    SH3DSP = 0x01a3,
    SH3E = 0x01a4,  // SH3E little-endian
    SH4 = 0x01a6,   // SH4 little-endian
    SH5 = 0x01a8,   // SH5
    ARM = 0x01c0,   // ARM Little-Endian
    THUMB = 0x01c2, // ARM Thumb/Thumb-2 Little-Endian
    ARMNT = 0x01c4, // ARM Thumb-2 Little-Endian
    AM33 = 0x01d3,
    POWERPC = 0x01F0, // IBM PowerPC Little-Endian
    POWERPCFP = 0x01f1,
    IA64 = 0x0200,      // Intel 64
    MIPS16 = 0x0266,    // MIPS
    ALPHA64 = 0x0284,   // ALPHA64
    MIPSFPU = 0x0366,   // MIPS
    MIPSFPU16 = 0x0466, // MIPS
    AXP64 = ALPHA64,
    TRICORE = 0x0520, // Infineon
    CEF = 0x0CEF,
    EBC = 0x0EBC,   // EFI Byte Code
    AMD64 = 0x8664, // AMD64 (K8)
    M32R = 0x9041,  // M32R little-endian
    CEE = 0xC0EE,
};

#pragma pack(push, 4)

template <typename T>
struct PEOptionalHeaderBasePart2_t
{
};

template <>
struct PEOptionalHeaderBasePart2_t<std::uint32_t>
{
    std::uint32_t BaseOfData;
    std::uint32_t ImageBase;
};

template <>
struct PEOptionalHeaderBasePart2_t<std::uint64_t>
{
    std::uint64_t ImageBase;
};

template <typename T>
struct PEOptionalHeaderBasePart1_t
{
    enum
    {
        k_NumberOfDataDirectors = 16
    };

    uint16_t Magic;
    uint8_t MajorLinkerVersion;
    uint8_t MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
};

struct PEDirectory_t2
{
    std::uint32_t VirtualAddress;
    std::uint32_t Size;
};

template <typename T>
struct PEOptionalHeaderBasePart3_t : PEOptionalHeaderBasePart1_t<T>, PEOptionalHeaderBasePart2_t<T>
{
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    T SizeOfStackReserve;
    T SizeOfStackCommit;
    T SizeOfHeapReserve;
    T SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    PEDirectory_t2 DataDirectory[PEOptionalHeaderBasePart1_t<T>::k_NumberOfDataDirectors];
};

template <typename T>
struct PEOptionalHeader_t
{
};

template <>
struct PEOptionalHeader_t<std::uint32_t> : PEOptionalHeaderBasePart3_t<std::uint32_t>
{
    enum
    {
        k_Magic = 0x10b, // IMAGE_NT_OPTIONAL_HDR32_MAGIC
    };
};

template <>
struct PEOptionalHeader_t<std::uint64_t> : PEOptionalHeaderBasePart3_t<std::uint64_t>
{
    enum
    {
        k_Magic = 0x20b, // IMAGE_NT_OPTIONAL_HDR64_MAGIC
    };
};

struct PEFileHeader_t
{
    PEMachineType Machine;
    std::uint16_t NumberOfSections;
    std::uint32_t TimeDateStamp;
    std::uint32_t PointerToSymbolTable;
    std::uint32_t NumberOfSymbols;
    std::uint16_t SizeOfOptionalHeader;
    std::uint16_t Characteristics;
};

template <typename T>
struct PENTHeaders_t
{
    enum
    {
        k_Signature = 0x00004550, // IMAGE_NT_SIGNATURE
    };

    uint32_t Signature;
    PEFileHeader_t FileHeader;
    PEOptionalHeader_t<T> OptionalHeader;
};

struct PEDosHeader_t
{
    enum
    {
        k_Magic = 0x5A4D
    };

    std::uint16_t e_magic;    // Magic number ( k_Magic )
    std::uint16_t e_cblp;     // Bytes on last page of file
    std::uint16_t e_cp;       // Pages in file
    std::uint16_t e_crlc;     // Relocations
    std::uint16_t e_cparhdr;  // Size of header in paragraphs
    std::uint16_t e_minalloc; // Minimum extra paragraphs needed
    std::uint16_t e_maxalloc; // Maximum extra paragraphs needed
    std::uint16_t e_ss;       // Initial (relative) SS value
    std::uint16_t e_sp;       // Initial SP value
    std::uint16_t e_csum;     // Checksum
    std::uint16_t e_ip;       // Initial IP value
    std::uint16_t e_cs;       // Initial (relative) CS value
    std::uint16_t e_lfarlc;   // File address of relocation table
    std::uint16_t e_ovno;     // Overlay number
    std::uint16_t e_res[4];   // Reserved words
    std::uint16_t e_oemid;    // OEM identifier (for e_oeminfo)
    std::uint16_t e_oeminfo;  // OEM information; e_oemid specific
    std::uint16_t e_res2[10]; // Reserved words
    std::uint32_t e_lfanew;   // File address of new exe header
};

#pragma pack(pop)

#define IMAGE_SIZEOF_SHORT_NAME 8

#ifndef OS_WINDOWS
typedef struct _IMAGE_SECTION_HEADER
{
    std::uint8_t Name[IMAGE_SIZEOF_SHORT_NAME];
    union
    {
        std::uint32_t PhysicalAddress;
        std::uint32_t VirtualSize;
    } Misc;
    std::uint32_t VirtualAddress;
    std::uint32_t SizeOfRawData;
    std::uint32_t PointerToRawData;
    std::uint32_t PointerToRelocations;
    std::uint32_t PointerToLinenumbers;
    std::uint16_t NumberOfRelocations;
    std::uint16_t NumberOfLinenumbers;
    std::uint32_t Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;

typedef struct _IMAGE_EXPORT_DIRECTORY
{
    DWORD Characteristics;
    DWORD TimeDateStamp;
    WORD MajorVersion;
    WORD MinorVersion;
    DWORD Name;
    DWORD Base;
    DWORD NumberOfFunctions;
    DWORD NumberOfNames;
    DWORD AddressOfFunctions;
    DWORD AddressOfNames;
    DWORD AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;

typedef struct _IMAGE_BASE_RELOCATION
{
    DWORD VirtualAddress;
    DWORD SizeOfBlock;
    // WORD TypeOffset[1];
} IMAGE_BASE_RELOCATION, *PIMAGE_BASE_RELOCATION;

#define IMAGE_ORDINAL_FLAG64             0x8000000000000000
#define IMAGE_ORDINAL_FLAG32             0x80000000
#define IMAGE_ORDINAL64(Ordinal)         (Ordinal & 0xffff)
#define IMAGE_ORDINAL32(Ordinal)         (Ordinal & 0xffff)
#define IMAGE_SNAP_BY_ORDINAL64(Ordinal) ((Ordinal & IMAGE_ORDINAL_FLAG64) != 0)
#define IMAGE_SNAP_BY_ORDINAL32(Ordinal) ((Ordinal & IMAGE_ORDINAL_FLAG32) != 0)

typedef struct _IMAGE_IMPORT_BY_NAME
{
    WORD Hint;
    CHAR Name[1];
} IMAGE_IMPORT_BY_NAME, *PIMAGE_IMPORT_BY_NAME;

typedef struct _IMAGE_IMPORT_DESCRIPTOR
{
    // union
    //{
    //     DWORD Characteristics;    // 0 for terminating null import descriptor
    DWORD OriginalFirstThunk; // RVA to original unbound IAT (PIMAGE_THUNK_DATA)
    //} DUMMYUNIONNAME;
    DWORD TimeDateStamp; // 0 if not bound,
                         // -1 if bound, and real date\time stamp
                         //     in IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT (new BIND)
                         // O.W. date/time stamp of DLL bound to (Old BIND)

    DWORD ForwarderChain; // -1 if no forwarders
    DWORD Name;
    DWORD FirstThunk; // RVA to IAT (if bound this IAT has actual addresses)
} IMAGE_IMPORT_DESCRIPTOR, *PIMAGE_IMPORT_DESCRIPTOR;

typedef struct _IMAGE_THUNK_DATA64
{
    union
    {
        ULONGLONG ForwarderString; // PBYTE
        ULONGLONG Function;        // PDWORD
        ULONGLONG Ordinal;
        ULONGLONG AddressOfData; // PIMAGE_IMPORT_BY_NAME
    } u1;
} IMAGE_THUNK_DATA64, *PIMAGE_THUNK_DATA64;

typedef struct _IMAGE_THUNK_DATA32
{
    union
    {
        DWORD ForwarderString; // PBYTE
        DWORD Function;        // PDWORD
        DWORD Ordinal;
        DWORD AddressOfData; // PIMAGE_IMPORT_BY_NAME
    } u1;
} IMAGE_THUNK_DATA32, *PIMAGE_THUNK_DATA32;

#endif

// Template type definitions for architecture-specific thunk data
template <typename T>
struct thunk_data_traits;

template <>
struct thunk_data_traits<std::uint32_t>
{
    using type = IMAGE_THUNK_DATA32;
    static constexpr DWORD ordinal_flag = IMAGE_ORDINAL_FLAG32;

    static constexpr WORD ordinal_mask(DWORD ordinal)
    {
        return IMAGE_ORDINAL32(ordinal);
    }
    static constexpr bool snap_by_ordinal(DWORD ordinal)
    {
        return IMAGE_SNAP_BY_ORDINAL32(ordinal);
    }
};

template <>
struct thunk_data_traits<std::uint64_t>
{
    using type = IMAGE_THUNK_DATA64;
    static constexpr ULONGLONG ordinal_flag = IMAGE_ORDINAL_FLAG64;

    static constexpr WORD ordinal_mask(ULONGLONG ordinal)
    {
        return IMAGE_ORDINAL64(ordinal);
    }
    static constexpr bool snap_by_ordinal(ULONGLONG ordinal)
    {
        return IMAGE_SNAP_BY_ORDINAL64(ordinal);
    }
};

template <typename Traits>
struct SECTION_BASIC_INFORMATION
{
    typename Traits::PVOID BaseAddress;
    ULONG Attributes;
    LARGE_INTEGER Size;
};

template <typename Traits>
struct SECTION_IMAGE_INFORMATION
{
    typename Traits::PVOID TransferAddress;
    ULONG ZeroBits;
    typename Traits::SIZE_T MaximumStackSize;
    typename Traits::SIZE_T CommittedStackSize;
    ULONG SubSystemType;

    union
    {
        struct
        {
            USHORT SubSystemMinorVersion;
            USHORT SubSystemMajorVersion;
        };

        ULONG SubSystemVersion;
    };

    union
    {
        struct
        {
            USHORT MajorOperatingSystemVersion;
            USHORT MinorOperatingSystemVersion;
        };

        ULONG OperatingSystemVersion;
    };

    USHORT ImageCharacteristics;
    USHORT DllCharacteristics;
    PEMachineType Machine;
    BOOLEAN ImageContainsCode;

    union
    {
        UCHAR ImageFlags;

        struct
        {
            UCHAR ComPlusNativeReady : 1;
            UCHAR ComPlusILOnly : 1;
            UCHAR ImageDynamicallyRelocated : 1;
            UCHAR ImageMappedFlat : 1;
            UCHAR BaseBelow4gb : 1;
            UCHAR ComPlusPrefer32bit : 1;
            UCHAR Reserved : 2;
        };
    };

    ULONG LoaderFlags;
    ULONG ImageFileSize;
    ULONG CheckSum;
};

namespace winpe
{

    enum class pe_arch
    {
        pe32,
        pe64
    };

    inline std::variant<pe_arch, std::error_code> get_pe_arch(const std::filesystem::path& file)
    {
        std::ifstream f(file, std::ios::binary);
        if (!f)
        {
            return std::make_error_code(std::errc::no_such_file_or_directory);
        }

        PEDosHeader_t dos{};
        f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
        if (!f || dos.e_magic != PEDosHeader_t::k_Magic)
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        f.seekg(dos.e_lfanew, std::ios::beg);
        uint32_t nt_signature = 0;
        f.read(reinterpret_cast<char*>(&nt_signature), sizeof(nt_signature));
        if (!f || nt_signature != PENTHeaders_t<std::uint32_t>::k_Signature)
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        PEFileHeader_t file_header{};
        f.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
        if (!f)
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        uint16_t magic = 0;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (!f)
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        if (magic == PEOptionalHeader_t<std::uint32_t>::k_Magic)
        {
            return pe_arch::pe32;
        }
        if (magic == PEOptionalHeader_t<std::uint64_t>::k_Magic)
        {
            return pe_arch::pe64;
        }

        return std::make_error_code(std::errc::executable_format_error);
    }

    inline std::variant<pe_arch, std::error_code> get_pe_arch(uint64_t base_address, uint64_t image_size)
    {
        const auto* base = reinterpret_cast<const std::byte*>(reinterpret_cast<const void*>(static_cast<uintptr_t>(base_address)));
        const uint64_t size = image_size;

        auto read = [&](uint64_t off, void* dst, size_t n) -> bool {
            if (off > size)
            {
                return false;
            }
            if (n > size - off)
            {
                return false;
            }
            memcpy(dst, base + off, n);
            return true;
        };

        PEDosHeader_t dos{};
        if (!read(0, &dos, sizeof(dos)) || dos.e_magic != PEDosHeader_t::k_Magic)
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        const auto nt_off = static_cast<uint64_t>(dos.e_lfanew);
        uint32_t nt_signature = 0;
        if (!read(nt_off, &nt_signature, sizeof(nt_signature)) || nt_signature != PENTHeaders_t<std::uint32_t>::k_Signature)
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        PEFileHeader_t file_header{};
        const uint64_t fh_off = nt_off + sizeof(nt_signature);
        if (!read(fh_off, &file_header, sizeof(file_header)))
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        uint16_t magic = 0;
        const uint64_t opt_magic_off = fh_off + sizeof(file_header);
        if (!read(opt_magic_off, &magic, sizeof(magic)))
        {
            return std::make_error_code(std::errc::executable_format_error);
        }

        if (magic == PEOptionalHeader_t<std::uint32_t>::k_Magic)
        {
            return pe_arch::pe32;
        }
        if (magic == PEOptionalHeader_t<std::uint64_t>::k_Magic)
        {
            return pe_arch::pe64;
        }

        return std::make_error_code(std::errc::executable_format_error);
    }

}

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
