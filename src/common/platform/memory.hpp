#pragma once

#define PAGE_EXECUTE                0x10
#define PAGE_EXECUTE_READ           0x20
#define PAGE_EXECUTE_READWRITE      0x40
#define PAGE_EXECUTE_WRITECOPY      0x80

#define PAGE_NOACCESS               0x01
#define PAGE_READONLY               0x02
#define PAGE_READWRITE              0x04
#define PAGE_WRITECOPY              0x08

#define PAGE_TARGETS_INVALID        0x40000000
#define PAGE_TARGETS_NO_UPDATE      0x40000000

#define PAGE_GUARD                  0x100
#define PAGE_NOCACHE                0x200
#define PAGE_WRITECOMBINE           0x400

#define MEM_COMMIT                  0x00001000
#define MEM_RESERVE                 0x00002000
#define MEM_DECOMMIT                0x00004000
#define MEM_RELEASE                 0x00008000
#define MEM_FREE                    0x00010000
#define MEM_PRIVATE                 0x00020000
#define MEM_MAPPED                  0x00040000
#define MEM_RESET                   0x00080000
#define MEM_TOP_DOWN                0x00100000
#define MEM_WRITE_WATCH             0x00200000
#define MEM_PHYSICAL                0x00400000
#define MEM_ROTATE                  0x00800000
#define MEM_DIFFERENT_IMAGE_BASE_OK 0x00800000
#define MEM_RESET_UNDO              0x01000000
#define MEM_LARGE_PAGES             0x20000000
#define MEM_DOS_LIM                 0x40000000
#define MEM_4MB_PAGES               0x80000000
#define MEM_64K_PAGES               (MEM_LARGE_PAGES | MEM_PHYSICAL)

typedef enum _MEMORY_INFORMATION_CLASS
{
    MemoryBasicInformation,              // q: MEMORY_BASIC_INFORMATION
    MemoryWorkingSetInformation,         // q: MEMORY_WORKING_SET_INFORMATION
    MemoryMappedFilenameInformation,     // q: UNICODE_STRING
    MemoryRegionInformation,             // q: MEMORY_REGION_INFORMATION
    MemoryWorkingSetExInformation,       // q: MEMORY_WORKING_SET_EX_INFORMATION // since VISTA
    MemorySharedCommitInformation,       // q: MEMORY_SHARED_COMMIT_INFORMATION // since WIN8
    MemoryImageInformation,              // q: MEMORY_IMAGE_INFORMATION
    MemoryRegionInformationEx,           // MEMORY_REGION_INFORMATION
    MemoryPrivilegedBasicInformation,    // MEMORY_BASIC_INFORMATION
    MemoryEnclaveImageInformation,       // MEMORY_ENCLAVE_IMAGE_INFORMATION // since REDSTONE3
    MemoryBasicInformationCapped,        // 10
    MemoryPhysicalContiguityInformation, // MEMORY_PHYSICAL_CONTIGUITY_INFORMATION // since 20H1
    MemoryBadInformation,                // since WIN11
    MemoryBadInformationAllProcesses,    // since 22H1
    MemoryImageExtensionInformation,     // since 24H2
    MaxMemoryInfoClass
} MEMORY_INFORMATION_CLASS;

typedef enum _SECTION_INHERIT
{
    ViewShare = 1,
    ViewUnmap = 2
} SECTION_INHERIT;

typedef struct DECLSPEC_ALIGN(16) _EMU_MEMORY_BASIC_INFORMATION64
{
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    std::int64_t RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} EMU_MEMORY_BASIC_INFORMATION64, *PEMU_MEMORY_BASIC_INFORMATION64;

typedef struct _MEMORY_IMAGE_INFORMATION64
{
    void* ImageBase;
    std::int64_t SizeOfImage;

    union
    {
        ULONG ImageFlags;

        struct
        {
            ULONG ImagePartialMap : 1;
            ULONG ImageNotExecutable : 1;
            ULONG ImageSigningLevel : 4;     // REDSTONE3
            ULONG ImageExtensionPresent : 1; // since 24H2
            ULONG Reserved : 25;
        };
    };
} MEMORY_IMAGE_INFORMATION64, *PMEMORY_IMAGE_INFORMATION64;

typedef struct _MEMORY_REGION_INFORMATION
{
    void* AllocationBase;
    ULONG AllocationProtect;

    union
    {
        ULONG RegionType;

        struct
        {
            ULONG Private : 1;
            ULONG MappedDataFile : 1;
            ULONG MappedImage : 1;
            ULONG MappedPageFile : 1;
            ULONG MappedPhysical : 1;
            ULONG DirectMapped : 1;
            ULONG SoftwareEnclave : 1; // REDSTONE3
            ULONG PageSize64K : 1;
            ULONG PlaceholderReservation : 1; // REDSTONE4
            ULONG MappedAwe : 1;              // 21H1
            ULONG MappedWriteWatch : 1;
            ULONG PageSizeLarge : 1;
            ULONG PageSizeHuge : 1;
            ULONG Reserved : 19;
        };
    };

    std::int64_t RegionSize;
    std::int64_t CommitSize;
    DWORD64 PartitionId;    // 19H1
    DWORD64 NodePreference; // 20H1
} MEMORY_REGION_INFORMATION64, *PMEMORY_REGION_INFORMATION64;
