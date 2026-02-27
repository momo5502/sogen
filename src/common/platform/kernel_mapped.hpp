#pragma once

#include <cstdint>

#include "primitives.hpp"
#include "traits.hpp"
#include "unicode.hpp"
#include "status.hpp"
#include "process.hpp"

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((static_cast<std::int32_t>(Status)) >= 0)
#endif

#define PROCESSOR_FEATURE_MAX                                           64
#define GDI_HANDLE_BUFFER_SIZE32                                        34
#define GDI_HANDLE_BUFFER_SIZE64                                        60
#define RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_RELEASE_ON_DEACTIVATION 0x00000001
#define RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_NO_DEACTIVATE           0x00000002
#define RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_ON_FREE_LIST            0x00000004
#define RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_HEAP_ALLOCATED          0x00000008
#define RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_NOT_REALLY_ACTIVATED    0x00000010
#define ACTIVATION_CONTEXT_STACK_FLAG_QUERIES_DISABLED                  0x00000001
#define GDI_BATCH_BUFFER_SIZE                                           310
#define WIN32_CLIENT_INFO_LENGTH                                        62
#define STATIC_UNICODE_BUFFER_LENGTH                                    261
#define TLS_MINIMUM_AVAILABLE                                           64
#define TLS_EXPANSION_SLOTS                                             1024

#ifndef OS_WINDOWS
#define PF_FLOATING_POINT_PRECISION_ERRATA         0
#define PF_FLOATING_POINT_EMULATED                 1
#define PF_COMPARE_EXCHANGE_DOUBLE                 2
#define PF_MMX_INSTRUCTIONS_AVAILABLE              3
#define PF_PPC_MOVEMEM_64BIT_OK                    4
#define PF_ALPHA_BYTE_INSTRUCTIONS                 5
#define PF_XMMI_INSTRUCTIONS_AVAILABLE             6
#define PF_3DNOW_INSTRUCTIONS_AVAILABLE            7
#define PF_RDTSC_INSTRUCTION_AVAILABLE             8
#define PF_PAE_ENABLED                             9
#define PF_XMMI64_INSTRUCTIONS_AVAILABLE           10
#define PF_SSE_DAZ_MODE_AVAILABLE                  11
#define PF_NX_ENABLED                              12
#define PF_SSE3_INSTRUCTIONS_AVAILABLE             13
#define PF_COMPARE_EXCHANGE128                     14
#define PF_COMPARE64_EXCHANGE128                   15
#define PF_CHANNELS_ENABLED                        16
#define PF_XSAVE_ENABLED                           17
#define PF_ARM_VFP_32_REGISTERS_AVAILABLE          18
#define PF_ARM_NEON_INSTRUCTIONS_AVAILABLE         19
#define PF_SECOND_LEVEL_ADDRESS_TRANSLATION        20
#define PF_VIRT_FIRMWARE_ENABLED                   21
#define PF_RDWRFSGSBASE_AVAILABLE                  22
#define PF_FASTFAIL_AVAILABLE                      23
#define PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE        24
#define PF_ARM_64BIT_LOADSTORE_ATOMIC              25
#define PF_ARM_EXTERNAL_CACHE_AVAILABLE            26
#define PF_ARM_FMAC_INSTRUCTIONS_AVAILABLE         27
#define PF_RDRAND_INSTRUCTION_AVAILABLE            28
#define PF_ARM_V8_INSTRUCTIONS_AVAILABLE           29
#define PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE    30
#define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE     31
#define PF_RDTSCP_INSTRUCTION_AVAILABLE            32
#define PF_RDPID_INSTRUCTION_AVAILABLE             33
#define PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE   34
#define PF_MONITORX_INSTRUCTION_AVAILABLE          35
#define PF_SSSE3_INSTRUCTIONS_AVAILABLE            36
#define PF_SSE4_1_INSTRUCTIONS_AVAILABLE           37
#define PF_SSE4_2_INSTRUCTIONS_AVAILABLE           38
#define PF_AVX_INSTRUCTIONS_AVAILABLE              39
#define PF_AVX2_INSTRUCTIONS_AVAILABLE             40
#define PF_AVX512F_INSTRUCTIONS_AVAILABLE          41
#define PF_ERMS_AVAILABLE                          42
#define PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE       43
#define PF_ARM_V83_JSCVT_INSTRUCTIONS_AVAILABLE    44
#define PF_ARM_V83_LRCPC_INSTRUCTIONS_AVAILABLE    45
#define PF_ARM_SVE_INSTRUCTIONS_AVAILABLE          46
#define PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE         47
#define PF_ARM_SVE2_1_INSTRUCTIONS_AVAILABLE       48
#define PF_ARM_SVE_AES_INSTRUCTIONS_AVAILABLE      49
#define PF_ARM_SVE_PMULL128_INSTRUCTIONS_AVAILABLE 50
#define PF_ARM_SVE_BITPERM_INSTRUCTIONS_AVAILABLE  51
#define PF_ARM_SVE_BF16_INSTRUCTIONS_AVAILABLE     52
#define PF_ARM_SVE_EBF16_INSTRUCTIONS_AVAILABLE    53
#define PF_ARM_SVE_B16B16_INSTRUCTIONS_AVAILABLE   54
#define PF_ARM_SVE_SHA3_INSTRUCTIONS_AVAILABLE     55
#define PF_ARM_SVE_SM4_INSTRUCTIONS_AVAILABLE      56
#define PF_ARM_SVE_I8MM_INSTRUCTIONS_AVAILABLE     57
#define PF_ARM_SVE_F32MM_INSTRUCTIONS_AVAILABLE    58
#define PF_ARM_SVE_F64MM_INSTRUCTIONS_AVAILABLE    59
#define PF_BMI2_INSTRUCTIONS_AVAILABLE             60
#define PF_MOVDIR64B_INSTRUCTION_AVAILABLE         61
#define PF_ARM_LSE2_AVAILABLE                      62
#define PF_RESERVED_FEATURE                        63
#define PF_ARM_SHA3_INSTRUCTIONS_AVAILABLE         64
#define PF_ARM_SHA512_INSTRUCTIONS_AVAILABLE       65
#define PF_ARM_V82_I8MM_INSTRUCTIONS_AVAILABLE     66
#define PF_ARM_V82_FP16_INSTRUCTIONS_AVAILABLE     67
#define PF_ARM_V86_BF16_INSTRUCTIONS_AVAILABLE     68
#define PF_ARM_V86_EBF16_INSTRUCTIONS_AVAILABLE    69
#define PF_ARM_SME_INSTRUCTIONS_AVAILABLE          70
#define PF_ARM_SME2_INSTRUCTIONS_AVAILABLE         71
#define PF_ARM_SME2_1_INSTRUCTIONS_AVAILABLE       72
#define PF_ARM_SME2_2_INSTRUCTIONS_AVAILABLE       73
#define PF_ARM_SME_AES_INSTRUCTIONS_AVAILABLE      74
#define PF_ARM_SME_SBITPERM_INSTRUCTIONS_AVAILABLE 75
#define PF_ARM_SME_SF8MM4_INSTRUCTIONS_AVAILABLE   76
#define PF_ARM_SME_SF8MM8_INSTRUCTIONS_AVAILABLE   77
#define PF_ARM_SME_SF8DP2_INSTRUCTIONS_AVAILABLE   78
#define PF_ARM_SME_SF8DP4_INSTRUCTIONS_AVAILABLE   79
#define PF_ARM_SME_SF8FMA_INSTRUCTIONS_AVAILABLE   80
#define PF_ARM_SME_F8F32_INSTRUCTIONS_AVAILABLE    81
#define PF_ARM_SME_F8F16_INSTRUCTIONS_AVAILABLE    82
#define PF_ARM_SME_F16F16_INSTRUCTIONS_AVAILABLE   83
#define PF_ARM_SME_B16B16_INSTRUCTIONS_AVAILABLE   84
#define PF_ARM_SME_F64F64_INSTRUCTIONS_AVAILABLE   85
#define PF_ARM_SME_I16I64_INSTRUCTIONS_AVAILABLE   86
#define PF_ARM_SME_LUTv2_INSTRUCTIONS_AVAILABLE    87
#define PF_ARM_SME_FA64_INSTRUCTIONS_AVAILABLE     88
#endif

typedef struct _EMU_NT_TIB64
{
    EMULATOR_CAST(std::uint64_t, struct _EXCEPTION_REGISTRATION_RECORD*) ExceptionList;
    std::uint64_t StackBase;
    std::uint64_t StackLimit;
    std::uint64_t SubSystemTib;
    std::uint64_t FiberData;
    std::uint64_t ArbitraryUserPointer;
    EMULATOR_CAST(std::uint64_t, struct _EMU_NT_TIB64*) Self;
} EMU_NT_TIB64, *PEMU_NT_TIB64;

typedef struct _EMU_NT_TIB32
{
    EMULATOR_CAST(std::uint32_t, struct _EXCEPTION_REGISTRATION_RECORD*) ExceptionList;
    std::uint32_t StackBase;
    std::uint32_t StackLimit;
    std::uint32_t SubSystemTib;
    std::uint32_t FiberData;
    std::uint32_t ArbitraryUserPointer;
    EMULATOR_CAST(std::uint32_t, struct _EMU_NT_TIB32*) Self;
} EMU_NT_TIB32, *PEMU_NT_TIB32;

union PEB_BITFIELD_UNION
{
    BOOLEAN BitField;

    struct
    {
        BOOLEAN ImageUsesLargePages : 1;
        BOOLEAN IsProtectedProcess : 1;
        BOOLEAN IsImageDynamicallyRelocated : 1;
        BOOLEAN SkipPatchingUser32Forwarders : 1;
        BOOLEAN IsPackagedProcess : 1;
        BOOLEAN IsAppContainer : 1;
        BOOLEAN IsProtectedProcessLight : 1;
        BOOLEAN IsLongPathAwareProcess : 1;
    };
};

#ifndef OS_WINDOWS

typedef struct _LIST_ENTRY64
{
    ULONGLONG Flink;
    ULONGLONG Blink;
} LIST_ENTRY64, *PLIST_ENTRY64, *RESTRICTED_POINTER PRLIST_ENTRY64;

typedef struct _LIST_ENTRY32
{
    ULONG Flink;
    ULONG Blink;
} LIST_ENTRY32, *PLIST_ENTRY32, *RESTRICTED_POINTER PRLIST_ENTRY32;

typedef enum _PROCESS_MITIGATION_POLICY
{
    ProcessDEPPolicy,
    ProcessASLRPolicy,
    ProcessDynamicCodePolicy,
    ProcessStrictHandleCheckPolicy,
    ProcessSystemCallDisablePolicy,
    ProcessMitigationOptionsMask,
    ProcessExtensionPointDisablePolicy,
    ProcessControlFlowGuardPolicy,
    ProcessSignaturePolicy,
    ProcessFontDisablePolicy,
    ProcessImageLoadPolicy,
    ProcessSystemCallFilterPolicy,
    ProcessPayloadRestrictionPolicy,
    ProcessChildProcessPolicy,
    ProcessSideChannelIsolationPolicy,
    ProcessUserShadowStackPolicy,
    ProcessRedirectionTrustPolicy,
    ProcessUserPointerAuthPolicy,
    ProcessSEHOPPolicy,
    ProcessActivationContextTrustPolicy,
    MaxProcessMitigationPolicy
} PROCESS_MITIGATION_POLICY, *PPROCESS_MITIGATION_POLICY;

#define WOW64_SIZE_OF_80387_REGISTERS     80

#define WOW64_MAXIMUM_SUPPORTED_EXTENSION 512

typedef struct _WOW64_FLOATING_SAVE_AREA
{
    DWORD ControlWord;
    DWORD StatusWord;
    DWORD TagWord;
    DWORD ErrorOffset;
    DWORD ErrorSelector;
    DWORD DataOffset;
    DWORD DataSelector;
    BYTE RegisterArea[WOW64_SIZE_OF_80387_REGISTERS];
    DWORD Cr0NpxState;
} WOW64_FLOATING_SAVE_AREA;

typedef struct _WOW64_CONTEXT
{
    DWORD ContextFlags;
    DWORD Dr0;
    DWORD Dr1;
    DWORD Dr2;
    DWORD Dr3;
    DWORD Dr6;
    DWORD Dr7;
    WOW64_FLOATING_SAVE_AREA FloatSave;
    DWORD SegGs;
    DWORD SegFs;
    DWORD SegEs;
    DWORD SegDs;
    DWORD Edi;
    DWORD Esi;
    DWORD Ebx;
    DWORD Edx;
    DWORD Ecx;
    DWORD Eax;
    DWORD Ebp;
    DWORD Eip;
    DWORD SegCs;
    DWORD EFlags;
    DWORD Esp;
    DWORD SegSs;
    BYTE ExtendedRegisters[WOW64_MAXIMUM_SUPPORTED_EXTENSION];

} WOW64_CONTEXT;

#define MEM_EXTENDED_PARAMETER_GRAPHICS            0x00000001
#define MEM_EXTENDED_PARAMETER_NONPAGED            0x00000002
#define MEM_EXTENDED_PARAMETER_ZERO_PAGES_OPTIONAL 0x00000004
#define MEM_EXTENDED_PARAMETER_NONPAGED_LARGE      0x00000008
#define MEM_EXTENDED_PARAMETER_NONPAGED_HUGE       0x00000010
#define MEM_EXTENDED_PARAMETER_SOFT_FAULT_PAGES    0x00000020
#define MEM_EXTENDED_PARAMETER_EC_CODE             0x00000040
#define MEM_EXTENDED_PARAMETER_IMAGE_NO_HPAT       0x00000080

typedef enum MEM_EXTENDED_PARAMETER_TYPE
{
    MemExtendedParameterInvalidType = 0,
    MemExtendedParameterAddressRequirements,
    MemExtendedParameterNumaNode,
    MemExtendedParameterPartitionHandle,
    MemExtendedParameterUserPhysicalHandle,
    MemExtendedParameterAttributeFlags,
    MemExtendedParameterImageMachine,
    MemExtendedParameterMax
} MEM_EXTENDED_PARAMETER_TYPE, *PMEM_EXTENDED_PARAMETER_TYPE;

#endif

typedef struct _PEB_LDR_DATA64
{
    ULONG Length;
    BOOLEAN Initialized;
    EmulatorTraits<Emu64>::HANDLE SsHandle;
    LIST_ENTRY64 InLoadOrderModuleList;
    LIST_ENTRY64 InMemoryOrderModuleList;
    LIST_ENTRY64 InInitializationOrderModuleList;
    std::uint64_t EntryInProgress;
    BOOLEAN ShutdownInProgress;
    EmulatorTraits<Emu64>::HANDLE ShutdownThreadId;
} PEB_LDR_DATA64, *PPEB_LDR_DATA64;

typedef struct _PEB_LDR_DATA32
{
    ULONG Length;
    BOOLEAN Initialized;
    EmulatorTraits<Emu32>::HANDLE SsHandle;
    LIST_ENTRY32 InLoadOrderModuleList;
    LIST_ENTRY32 InMemoryOrderModuleList;
    LIST_ENTRY32 InInitializationOrderModuleList;
    std::uint32_t EntryInProgress;
    BOOLEAN ShutdownInProgress;
    EmulatorTraits<Emu32>::HANDLE ShutdownThreadId;
} PEB_LDR_DATA32, *PPEB_LDR_DATA32;

static_assert(sizeof(PEB_LDR_DATA32) == 48);

using STRING64 = UNICODE_STRING<EmulatorTraits<Emu64>>;
using ANSI_STRING64 = STRING64;
using OEM_STRING64 = STRING64;

typedef struct _RTL_DRIVE_LETTER_CURDIR64
{
    USHORT Flags;
    USHORT Length;
    ULONG TimeStamp;
    STRING64 DosPath;
} RTL_DRIVE_LETTER_CURDIR64, *PRTL_DRIVE_LETTER_CURDIR64;

#define RTL_MAX_DRIVE_LETTERS  32
#define RTL_DRIVE_LETTER_VALID (USHORT)0x0001

template <typename T, size_t Size>
struct ARRAY_CONTAINER
{
    T arr[Size];
};

typedef struct _CURDIR64
{
    UNICODE_STRING<EmulatorTraits<Emu64>> DosPath;
    EmulatorTraits<Emu64>::HANDLE Handle;
} CURDIR64, *PCURDIR64;

#define RTL_USER_PROCESS_PARAMETERS_NORMALIZED            0x01
#define RTL_USER_PROCESS_PARAMETERS_PROFILE_USER          0x02
#define RTL_USER_PROCESS_PARAMETERS_PROFILE_KERNEL        0x04
#define RTL_USER_PROCESS_PARAMETERS_PROFILE_SERVER        0x08
#define RTL_USER_PROCESS_PARAMETERS_RESERVE_1MB           0x20
#define RTL_USER_PROCESS_PARAMETERS_RESERVE_16MB          0x40
#define RTL_USER_PROCESS_PARAMETERS_CASE_SENSITIVE        0x80
#define RTL_USER_PROCESS_PARAMETERS_DISABLE_HEAP_DECOMMIT 0x100
#define RTL_USER_PROCESS_PARAMETERS_DLL_REDIRECTION_LOCAL 0x1000
#define RTL_USER_PROCESS_PARAMETERS_APP_MANIFEST_PRESENT  0x2000
#define RTL_USER_PROCESS_PARAMETERS_IMAGE_KEY_MISSING     0x4000
#define RTL_USER_PROCESS_PARAMETERS_NX_OPTIN              0x20000

typedef struct _RTL_USER_PROCESS_PARAMETERS64
{
    ULONG MaximumLength;
    ULONG Length;

    ULONG Flags;
    ULONG DebugFlags;

    EmulatorTraits<Emu64>::HANDLE ConsoleHandle;
    ULONG ConsoleFlags;
    EmulatorTraits<Emu64>::HANDLE StandardInput;
    EmulatorTraits<Emu64>::HANDLE StandardOutput;
    EmulatorTraits<Emu64>::HANDLE StandardError;

    CURDIR64 CurrentDirectory;
    UNICODE_STRING<EmulatorTraits<Emu64>> DllPath;
    UNICODE_STRING<EmulatorTraits<Emu64>> ImagePathName;
    UNICODE_STRING<EmulatorTraits<Emu64>> CommandLine;
    std::uint64_t Environment;

    ULONG StartingX;
    ULONG StartingY;
    ULONG CountX;
    ULONG CountY;
    ULONG CountCharsX;
    ULONG CountCharsY;
    ULONG FillAttribute;

    ULONG WindowFlags;
    ULONG ShowWindowFlags;
    UNICODE_STRING<EmulatorTraits<Emu64>> WindowTitle;
    UNICODE_STRING<EmulatorTraits<Emu64>> DesktopInfo;
    UNICODE_STRING<EmulatorTraits<Emu64>> ShellInfo;
    UNICODE_STRING<EmulatorTraits<Emu64>> RuntimeData;
    ARRAY_CONTAINER<RTL_DRIVE_LETTER_CURDIR64, RTL_MAX_DRIVE_LETTERS> CurrentDirectories;

    std::uint64_t EnvironmentSize;
    std::uint64_t EnvironmentVersion;

    std::uint64_t PackageDependencyData;
    ULONG ProcessGroupId;
    ULONG LoaderThreads;

    UNICODE_STRING<EmulatorTraits<Emu64>> RedirectionDllName; // REDSTONE4
    UNICODE_STRING<EmulatorTraits<Emu64>> HeapPartitionName;  // 19H1
    std::uint64_t DefaultThreadpoolCpuSetMasks;
    ULONG DefaultThreadpoolCpuSetMaskCount;
    ULONG DefaultThreadpoolThreadMaximum;
    ULONG HeapMemoryTypeMask; // WIN11
} RTL_USER_PROCESS_PARAMETERS64, *PRTL_USER_PROCESS_PARAMETERS64;

static_assert(sizeof(RTL_USER_PROCESS_PARAMETERS64) == 0x448);

union PEB_CROSS_PROCESS_FLAGS_UNION
{
    ULONG CrossProcessFlags;

    struct
    {
        ULONG ProcessInJob : 1;
        ULONG ProcessInitializing : 1;
        ULONG ProcessUsingVEH : 1;
        ULONG ProcessUsingVCH : 1;
        ULONG ProcessUsingFTH : 1;
        ULONG ProcessPreviouslyThrottled : 1;
        ULONG ProcessCurrentlyThrottled : 1;
        ULONG ProcessImagesHotPatched : 1; // REDSTONE5
        ULONG ReservedBits0 : 24;
    };
};

union PEB_KERNEL_CALLBACK_TABLE_UNION64
{
    std::uint64_t KernelCallbackTable;
    std::uint64_t UserSharedInfoPtr;
};

typedef struct _API_SET_NAMESPACE
{
    ULONG Version;
    ULONG Size;
    ULONG Flags;
    ULONG Count;
    ULONG EntryOffset;
    ULONG HashOffset;
    ULONG HashFactor;
} API_SET_NAMESPACE, *PAPI_SET_NAMESPACE;

union PEB_CONTEXT_DATA_UNION64
{
    std::uint64_t pContextData; // WIN7
    std::uint64_t pUnused;      // WIN10
    std::uint64_t EcCodeBitMap; // WIN11
};

union PEB_TRACING_FLAGS_UNION
{
    ULONG TracingFlags;

    struct
    {
        ULONG HeapTracingEnabled : 1;
        ULONG CritSecTracingEnabled : 1;
        ULONG LibLoaderTracingEnabled : 1;
        ULONG SpareTracingBits : 29;
    };
};

union PEB_LEAP_SECONDS_FLAG_UNION
{
    ULONG LeapSecondFlags;

    struct
    {
        ULONG SixtySecondEnabled : 1;
        ULONG Reserved : 31;
    };
};

#define MAXIMUM_LEADBYTES 12

typedef struct _CPTABLEINFO
{
    USHORT CodePage;
    USHORT MaximumCharacterSize;
    USHORT DefaultChar;
    USHORT UniDefaultChar;
    USHORT TransDefaultChar;
    USHORT TransUniDefaultChar;
    USHORT DBCSCodePage;
    UCHAR LeadByte[MAXIMUM_LEADBYTES];
    EMULATOR_CAST(uint64_t, USHORT*) MultiByteTable;
    EMULATOR_CAST(uint64_t, void*) WideCharTable;
    EMULATOR_CAST(uint64_t, USHORT*) DBCSRanges;
    EMULATOR_CAST(uint64_t, USHORT*) DBCSOffsets;
} CPTABLEINFO, *PCPTABLEINFO;

typedef struct _NLSTABLEINFO
{
    CPTABLEINFO OemTableInfo;
    CPTABLEINFO AnsiTableInfo;
    EMULATOR_CAST(uint64_t, USHORT*) UpperCaseTable;
    EMULATOR_CAST(uint64_t, USHORT*) LowerCaseTable;
} NLSTABLEINFO, *PNLSTABLEINFO;

typedef struct _CURDIR32
{
    UNICODE_STRING<EmulatorTraits<Emu32>> DosPath;
    EmulatorTraits<Emu32>::HANDLE Handle;
} CURDIR32, *PCURDIR32;

static_assert(sizeof(CURDIR32) == 12);

typedef struct _RTL_DRIVE_LETTER_CURDIR32
{
    USHORT Flags;
    USHORT Length;
    ULONG TimeStamp;
    UNICODE_STRING<EmulatorTraits<Emu32>> DosPath;
} RTL_DRIVE_LETTER_CURDIR32, *PRTL_DRIVE_LETTER_CURDIR32;

static_assert(sizeof(RTL_DRIVE_LETTER_CURDIR32) == 16);

typedef struct _RTL_USER_PROCESS_PARAMETERS32
{
    ULONG MaximumLength;
    ULONG Length;

    ULONG Flags;
    ULONG DebugFlags;

    EmulatorTraits<Emu32>::HANDLE ConsoleHandle;
    ULONG ConsoleFlags;
    EmulatorTraits<Emu32>::HANDLE StandardInput;
    EmulatorTraits<Emu32>::HANDLE StandardOutput;
    EmulatorTraits<Emu32>::HANDLE StandardError;

    CURDIR32 CurrentDirectory;
    UNICODE_STRING<EmulatorTraits<Emu32>> DllPath;
    UNICODE_STRING<EmulatorTraits<Emu32>> ImagePathName;
    UNICODE_STRING<EmulatorTraits<Emu32>> CommandLine;
    std::uint32_t Environment;

    ULONG StartingX;
    ULONG StartingY;
    ULONG CountX;
    ULONG CountY;
    ULONG CountCharsX;
    ULONG CountCharsY;
    ULONG FillAttribute;

    ULONG WindowFlags;
    ULONG ShowWindowFlags;
    UNICODE_STRING<EmulatorTraits<Emu32>> WindowTitle;
    UNICODE_STRING<EmulatorTraits<Emu32>> DesktopInfo;
    UNICODE_STRING<EmulatorTraits<Emu32>> ShellInfo;
    UNICODE_STRING<EmulatorTraits<Emu32>> RuntimeData;
    RTL_DRIVE_LETTER_CURDIR32 CurrentDirectories[RTL_MAX_DRIVE_LETTERS];

    std::uint32_t EnvironmentSize;
    std::uint32_t EnvironmentVersion;

    std::uint32_t PackageDependencyData;
    ULONG ProcessGroupId;
    ULONG LoaderThreads;

    UNICODE_STRING<EmulatorTraits<Emu32>> RedirectionDllName; // REDSTONE4
    UNICODE_STRING<EmulatorTraits<Emu32>> HeapPartitionName;  // 19H1
    std::uint32_t DefaultThreadpoolCpuSetMasks;
    ULONG DefaultThreadpoolCpuSetMaskCount;
    ULONG DefaultThreadpoolThreadMaximum;
    ULONG HeapMemoryTypeMask; // WIN11
} RTL_USER_PROCESS_PARAMETERS32, *PRTL_USER_PROCESS_PARAMETERS32;

static_assert(sizeof(RTL_USER_PROCESS_PARAMETERS32) == 708);

typedef struct _PEB64
{
    BOOLEAN InheritedAddressSpace;
    BOOLEAN ReadImageFileExecOptions;
    BOOLEAN BeingDebugged;
    PEB_BITFIELD_UNION BitField;

    EmulatorTraits<Emu64>::HANDLE Mutant;

    std::uint64_t ImageBaseAddress;
    EMULATOR_CAST(std::uint64_t, PPEB_LDR_DATA64) Ldr;
    EMULATOR_CAST(std::uint64_t, PRTL_USER_PROCESS_PARAMETERS64) ProcessParameters;
    std::uint64_t SubSystemData;
    std::uint64_t ProcessHeap;
    EMULATOR_CAST(std::uint64_t, PRTL_CRITICAL_SECTION) FastPebLock;
    EMULATOR_CAST(std::uint64_t, PSLIST_HEADER) AtlThunkSListPtr;
    std::uint64_t IFEOKey;
    PEB_CROSS_PROCESS_FLAGS_UNION CrossProcessFlags;
    PEB_KERNEL_CALLBACK_TABLE_UNION64 KernelCallbackTable;

    ULONG SystemReserved;
    ULONG AtlThunkSListPtr32;
    EMULATOR_CAST(std::uint64_t, PAPI_SET_NAMESPACE) ApiSetMap;
    ULONG TlsExpansionCounter;
    EMULATOR_CAST(std::uint64_t, PRTL_BITMAP) TlsBitmap;

    ARRAY_CONTAINER<ULONG, 2> TlsBitmapBits; // TLS_MINIMUM_AVAILABLE
    std::uint64_t ReadOnlySharedMemoryBase;
    EMULATOR_CAST(std::uint64_t, PSILO_USER_SHARED_DATA) SharedData; // HotpatchInformation
    std::uint64_t ReadOnlyStaticServerData;

    EMULATOR_CAST(EmulatorTraits<Emu64>::PVOID, PCPTABLEINFO) AnsiCodePageData;      // PCPTABLEINFO
    EMULATOR_CAST(EmulatorTraits<Emu64>::PVOID, PCPTABLEINFO) OemCodePageData;       // PCPTABLEINFO
    EMULATOR_CAST(EmulatorTraits<Emu64>::PVOID, PNLSTABLEINFO) UnicodeCaseTableData; // PNLSTABLEINFO

    ULONG NumberOfProcessors;
    ULONG NtGlobalFlag;

    ULARGE_INTEGER CriticalSectionTimeout;
    EMULATOR_CAST(std::uint64_t, SIZE_T) HeapSegmentReserve;
    EMULATOR_CAST(std::uint64_t, SIZE_T) HeapSegmentCommit;
    EMULATOR_CAST(std::uint64_t, SIZE_T) HeapDeCommitTotalFreeThreshold;
    EMULATOR_CAST(std::uint64_t, SIZE_T) HeapDeCommitFreeBlockThreshold;

    ULONG NumberOfHeaps;
    ULONG MaximumNumberOfHeaps;
    std::uint64_t ProcessHeaps; // PHEAP

    std::uint64_t GdiSharedHandleTable; // PGDI_SHARED_MEMORY
    std::uint64_t ProcessStarterHelper;
    ULONG GdiDCAttributeList;

    EMULATOR_CAST(std::uint64_t, PRTL_CRITICAL_SECTION) LoaderLock;

    ULONG OSMajorVersion;
    ULONG OSMinorVersion;
    USHORT OSBuildNumber;
    USHORT OSCSDVersion;
    ULONG OSPlatformId;
    ULONG ImageSubsystem;
    ULONG ImageSubsystemMajorVersion;
    ULONG ImageSubsystemMinorVersion;
    EMULATOR_CAST(std::uint64_t, KAFFINITY) ActiveProcessAffinityMask;
    ARRAY_CONTAINER<ULONG, GDI_HANDLE_BUFFER_SIZE64> GdiHandleBuffer;
    std::uint64_t PostProcessInitRoutine;

    EMULATOR_CAST(std::uint64_t, PRTL_BITMAP) TlsExpansionBitmap;
    ARRAY_CONTAINER<ULONG, 32> TlsExpansionBitmapBits; // TLS_EXPANSION_SLOTS

    ULONG SessionId;

    ULARGE_INTEGER AppCompatFlags; // KACF_*
    ULARGE_INTEGER AppCompatFlagsUser;
    std::uint64_t pShimData;
    std::uint64_t AppCompatInfo; // APPCOMPAT_EXE_DATA

    UNICODE_STRING<EmulatorTraits<Emu64>> CSDVersion;

    EMULATOR_CAST(std::uint64_t, PACTIVATION_CONTEXT_DATA) ActivationContextData;
    EMULATOR_CAST(std::uint64_t, PASSEMBLY_STORAGE_MAP) ProcessAssemblyStorageMap;
    EMULATOR_CAST(std::uint64_t, PACTIVATION_CONTEXT_DATA) SystemDefaultActivationContextData;
    EMULATOR_CAST(std::uint64_t, PASSEMBLY_STORAGE_MAP) SystemAssemblyStorageMap;

    EMULATOR_CAST(std::uint64_t, SIZE_T) MinimumStackCommit;

    ARRAY_CONTAINER<std::uint64_t, 2> SparePointers; // 19H1 (previously FlsCallback to FlsHighIndex)
    std::uint64_t PatchLoaderData;
    std::uint64_t ChpeV2ProcessInfo; // _CHPEV2_PROCESS_INFO

    ULONG AppModelFeatureState;
    ARRAY_CONTAINER<ULONG, 2> SpareUlongs;

    USHORT ActiveCodePage;
    USHORT OemCodePage;
    USHORT UseCaseMapping;
    USHORT UnusedNlsField;

    std::uint64_t WerRegistrationData;
    std::uint64_t WerShipAssertPtr;

    PEB_CONTEXT_DATA_UNION64 ContextData;

    std::uint64_t pImageHeaderHash;
    PEB_TRACING_FLAGS_UNION TracingFlags;

    ULONGLONG CsrServerReadOnlySharedMemoryBase;
    EMULATOR_CAST(std::uint64_t, PRTL_CRITICAL_SECTION) TppWorkerpListLock;
    LIST_ENTRY64 TppWorkerpList;
    ARRAY_CONTAINER<std::uint64_t, 128> WaitOnAddressHashTable;
    EMULATOR_CAST(std::uint64_t, PTELEMETRY_COVERAGE_HEADER) TelemetryCoverageHeader; // REDSTONE3
    ULONG CloudFileFlags;
    ULONG CloudFileDiagFlags; // REDSTONE4
    CHAR PlaceholderCompatibilityMode;
    ARRAY_CONTAINER<CHAR, 7> PlaceholderCompatibilityModeReserved;
    EMULATOR_CAST(std::uint64_t, PLEAP_SECOND_DATA) LeapSecondData; // REDSTONE5
    PEB_LEAP_SECONDS_FLAG_UNION LeapSecondFlags;

    ULONG NtGlobalFlag2;
    ULONGLONG ExtendedFeatureDisableMask; // since WIN11
} PEB64, *PPEB64;

static_assert(sizeof(PEB64) == 0x7D0);

typedef struct _PEB32
{
    BOOLEAN InheritedAddressSpace;
    BOOLEAN ReadImageFileExecOptions;
    BOOLEAN BeingDebugged;
    union
    {
        BOOLEAN BitField;
        struct
        {
            BOOLEAN ImageUsesLargePages : 1;
            BOOLEAN IsProtectedProcess : 1;
            BOOLEAN IsImageDynamicallyRelocated : 1;
            BOOLEAN SkipPatchingUser32Forwarders : 1;
            BOOLEAN IsPackagedProcess : 1;
            BOOLEAN IsAppContainer : 1;
            BOOLEAN IsProtectedProcessLight : 1;
            BOOLEAN IsLongPathAwareProcess : 1;
        };
    };

    EmulatorTraits<Emu32>::HANDLE Mutant;

    std::uint32_t ImageBaseAddress;
    EMULATOR_CAST(std::uint32_t, PPEB_LDR_DATA32) Ldr;
    EMULATOR_CAST(std::uint32_t, struct _RTL_USER_PROCESS_PARAMETERS32*) ProcessParameters;
    EMULATOR_CAST(std::uint32_t, PVOID32) SubSystemData;
    EMULATOR_CAST(std::uint32_t, PVOID32) ProcessHeap;
    EMULATOR_CAST(std::uint32_t, struct _RTL_CRITICAL_SECTION32*) FastPebLock;
    EMULATOR_CAST(std::uint32_t, union _SLIST_HEADER*) AtlThunkSListPtr;
    EMULATOR_CAST(std::uint32_t, PVOID32) IFEOKey;

    union
    {
        ULONG CrossProcessFlags;
        struct
        {
            ULONG ProcessInJob : 1;
            ULONG ProcessInitializing : 1;
            ULONG ProcessUsingVEH : 1;
            ULONG ProcessUsingVCH : 1;
            ULONG ProcessUsingFTH : 1;
            ULONG ProcessPreviouslyThrottled : 1;
            ULONG ProcessCurrentlyThrottled : 1;
            ULONG ProcessImagesHotPatched : 1; // REDSTONE5
            ULONG ReservedBits0 : 24;
        };
    };
    union
    {
        EMULATOR_CAST(std::uint32_t, PVOID32) KernelCallbackTable;
        EMULATOR_CAST(std::uint32_t, PVOID32) UserSharedInfoPtr;
    };
    ULONG SystemReserved;
    ULONG AtlThunkSListPtr32;
    EMULATOR_CAST(std::uint32_t, struct _API_SET_NAMESPACE*) ApiSetMap;
    ULONG TlsExpansionCounter;
    EMULATOR_CAST(std::uint32_t, struct _RTL_BITMAP*) TlsBitmap;
    ARRAY_CONTAINER<ULONG, 2> TlsBitmapBits;

    EMULATOR_CAST(std::uint32_t, PVOID32) ReadOnlySharedMemoryBase;
    EMULATOR_CAST(std::uint32_t, struct _SILO_USER_SHARED_DATA*) SharedData; // HotpatchInformation
    EMULATOR_CAST(std::uint32_t, PVOID32*) ReadOnlyStaticServerData;

    EMULATOR_CAST(std::uint32_t, PVOID32) AnsiCodePageData;     // PCPTABLEINFO
    EMULATOR_CAST(std::uint32_t, PVOID32) OemCodePageData;      // PCPTABLEINFO
    EMULATOR_CAST(std::uint32_t, PVOID32) UnicodeCaseTableData; // PNLSTABLEINFO

    ULONG NumberOfProcessors;
    ULONG NtGlobalFlag;

    ULARGE_INTEGER CriticalSectionTimeout;
    EMULATOR_CAST(std::uint32_t, SIZE_T32) HeapSegmentReserve;
    EMULATOR_CAST(std::uint32_t, SIZE_T32) HeapSegmentCommit;
    EMULATOR_CAST(std::uint32_t, SIZE_T32) HeapDeCommitTotalFreeThreshold;
    EMULATOR_CAST(std::uint32_t, SIZE_T32) HeapDeCommitFreeBlockThreshold;

    ULONG NumberOfHeaps;
    ULONG MaximumNumberOfHeaps;
    EMULATOR_CAST(std::uint32_t, PVOID32*) ProcessHeaps; // PHEAP

    EMULATOR_CAST(std::uint32_t, PVOID32) GdiSharedHandleTable; // PGDI_SHARED_MEMORY
    EMULATOR_CAST(std::uint32_t, PVOID32) ProcessStarterHelper;
    ULONG GdiDCAttributeList;

    EMULATOR_CAST(std::uint32_t, struct _RTL_CRITICAL_SECTION32*) LoaderLock;

    ULONG OSMajorVersion;
    ULONG OSMinorVersion;
    USHORT OSBuildNumber;
    USHORT OSCSDVersion;
    ULONG OSPlatformId;
    ULONG ImageSubsystem;
    ULONG ImageSubsystemMajorVersion;
    ULONG ImageSubsystemMinorVersion;
    EMULATOR_CAST(std::uint32_t, KAFFINITY32) ActiveProcessAffinityMask;
    ARRAY_CONTAINER<ULONG, GDI_HANDLE_BUFFER_SIZE32> GdiHandleBuffer;
    EMULATOR_CAST(std::uint32_t, PVOID32) PostProcessInitRoutine;

    EMULATOR_CAST(std::uint32_t, PVOID32) TlsExpansionBitmap;
    ARRAY_CONTAINER<ULONG, 32> TlsExpansionBitmapBits;

    ULONG SessionId;

    ULARGE_INTEGER AppCompatFlags;
    ULARGE_INTEGER AppCompatFlagsUser;
    EMULATOR_CAST(std::uint32_t, PVOID32) pShimData;
    EMULATOR_CAST(std::uint32_t, PVOID32) AppCompatInfo; // APPCOMPAT_EXE_DATA

    UNICODE_STRING<EmulatorTraits<Emu32>> CSDVersion;

    EMULATOR_CAST(std::uint32_t, struct _ACTIVATION_CONTEXT_DATA*) ActivationContextData;
    EMULATOR_CAST(std::uint32_t, struct _ASSEMBLY_STORAGE_MAP32*) ProcessAssemblyStorageMap;
    EMULATOR_CAST(std::uint32_t, struct _ACTIVATION_CONTEXT_DATA*) SystemDefaultActivationContextData;
    EMULATOR_CAST(std::uint32_t, struct _ASSEMBLY_STORAGE_MAP32*) SystemAssemblyStorageMap;

    EMULATOR_CAST(std::uint32_t, SIZE_T32) MinimumStackCommit;

    ARRAY_CONTAINER<ULONG, 2> SparePointers; // 19H1 (previously FlsCallback to FlsHighIndex)
    EMULATOR_CAST(std::uint32_t, PVOID32) PatchLoaderData;
    EMULATOR_CAST(std::uint32_t, PVOID32) ChpeV2ProcessInfo; // _CHPEV2_PROCESS_INFO

    ULONG AppModelFeatureState;
    ULONG SpareUlongs[2];

    USHORT ActiveCodePage;
    USHORT OemCodePage;
    USHORT UseCaseMapping;
    USHORT UnusedNlsField;

    EMULATOR_CAST(std::uint32_t, PVOID32) WerRegistrationData;
    EMULATOR_CAST(std::uint32_t, PVOID32) WerShipAssertPtr;

    union
    {
        EMULATOR_CAST(std::uint32_t, PVOID32) pContextData; // WIN7
        EMULATOR_CAST(std::uint32_t, PVOID32) pUnused;      // WIN10
        EMULATOR_CAST(std::uint32_t, PVOID32) EcCodeBitMap; // WIN11
    };

    EMULATOR_CAST(std::uint32_t, PVOID32) pImageHeaderHash;
    union
    {
        ULONG TracingFlags;
        struct
        {
            ULONG HeapTracingEnabled : 1;
            ULONG CritSecTracingEnabled : 1;
            ULONG LibLoaderTracingEnabled : 1;
            ULONG SpareTracingBits : 29;
        };
    };
    ULONGLONG CsrServerReadOnlySharedMemoryBase;
    EMULATOR_CAST(std::uint32_t, struct _RTL_CRITICAL_SECTION32*) TppWorkerpListLock;
    LIST_ENTRY32 TppWorkerpList;
    ARRAY_CONTAINER<ULONG, 128> WaitOnAddressHashTable;
    EMULATOR_CAST(std::uint32_t, struct _PTELEMETRY_COVERAGE_HEADER*) TelemetryCoverageHeader; // REDSTONE3
    ULONG CloudFileFlags;
    ULONG CloudFileDiagFlags; // REDSTONE4
    CHAR PlaceholderCompatibilityMode;
    ARRAY_CONTAINER<CHAR, 7> PlaceholderCompatibilityModeReserved;
    EMULATOR_CAST(std::uint32_t, struct _LEAP_SECOND_DATA*) LeapSecondData; // REDSTONE5
    union
    {
        ULONG LeapSecondFlags;
        struct
        {
            ULONG SixtySecondEnabled : 1;
            ULONG Reserved : 31;
        };
    };
    ULONG NtGlobalFlag2;
    ULONGLONG ExtendedFeatureDisableMask; // since WIN11

} PEB32, *PPEB32;

static_assert(sizeof(PEB32) == 0x488, "sizeof(PEB32) is incorrect"); // WIN11

typedef struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME64
{
    struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME* Previous;
    EMULATOR_CAST(std::uint64_t, ACTIVATION_CONTEXT) ActivationContext;
    ULONG Flags; // RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_*
} RTL_ACTIVATION_CONTEXT_STACK_FRAME64, *PRTL_ACTIVATION_CONTEXT_STACK_FRAME64;

typedef struct _ASSEMBLY_STORAGE_MAP_ENTRY32
{
    ULONG Flags;
    UNICODE_STRING<EmulatorTraits<Emu32>> DosPath;
    EMULATOR_CAST(std::uint32_t, HANDLE32) Handle;
} ASSEMBLY_STORAGE_MAP_ENTRY32, *PASSEMBLY_STORAGE_MAP_ENTRY32;

static_assert(sizeof(ASSEMBLY_STORAGE_MAP_ENTRY32) == 16);

typedef struct _ASSEMBLY_STORAGE_MAP32
{
    ULONG Flags;
    ULONG AssemblyCount;
    EMULATOR_CAST(std::uint32_t, PASSEMBLY_STORAGE_MAP_ENTRY32*) AssemblyArray;
} ASSEMBLY_STORAGE_MAP32, *PASSEMBLY_STORAGE_MAP32;

static_assert(sizeof(ASSEMBLY_STORAGE_MAP32) == 12);

typedef struct _ACTIVATION_CONTEXT32
{
    LONG RefCount;
    ULONG Flags;
    EMULATOR_CAST(std::uint32_t, struct _ACTIVATION_CONTEXT_DATA*) ActivationContextData;
    std::uint32_t /*PACTIVATION_CONTEXT_NOTIFY_ROUTINE*/ NotificationRoutine;
    std::uint32_t NotificationContext;
    ULONG SentNotifications[8];
    ULONG DisabledNotifications[8];
    ASSEMBLY_STORAGE_MAP32 StorageMap;
    EMULATOR_CAST(std::uint32_t, PASSEMBLY_STORAGE_MAP_ENTRY32) InlineStorageMapEntries[32];
} ACTIVATION_CONTEXT32, *PACTIVATION_CONTEXT32;

static_assert(sizeof(ACTIVATION_CONTEXT32) == 224);

typedef struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME32
{
    EMULATOR_CAST(std::uint32_t, struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME32*) Previous;
    EMULATOR_CAST(std::uint32_t, PACTIVATION_CONTEXT32) ActivationContext;
    ULONG Flags; // RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_*
} RTL_ACTIVATION_CONTEXT_STACK_FRAME32, *PRTL_ACTIVATION_CONTEXT_STACK_FRAME32;

static_assert(sizeof(RTL_ACTIVATION_CONTEXT_STACK_FRAME32) == 12);

typedef struct _ACTIVATION_CONTEXT_STACK32
{
    EMULATOR_CAST(std::uint32_t, PRTL_ACTIVATION_CONTEXT_STACK_FRAME32) ActiveFrame;
    LIST_ENTRY32 FrameListCache;
    ULONG Flags; // ACTIVATION_CONTEXT_STACK_FLAG_*
    ULONG NextCookieSequenceNumber;
    ULONG StackId;
} ACTIVATION_CONTEXT_STACK32, *PACTIVATION_CONTEXT_STACK32;

static_assert(sizeof(ACTIVATION_CONTEXT_STACK32) == 24);

typedef struct _ACTIVATION_CONTEXT_STACK64
{
    EMULATOR_CAST(std::uint64_t, PRTL_ACTIVATION_CONTEXT_STACK_FRAME64) ActiveFrame;
    LIST_ENTRY64 FrameListCache;
    ULONG Flags; // ACTIVATION_CONTEXT_STACK_FLAG_*
    ULONG NextCookieSequenceNumber;
    ULONG StackId;
} ACTIVATION_CONTEXT_STACK64, *PACTIVATION_CONTEXT_STACK64;

typedef struct _GDI_TEB_BATCH64
{
    ULONG Offset;
    std::uint64_t HDC;
    ULONG Buffer[GDI_BATCH_BUFFER_SIZE];
} GDI_TEB_BATCH64, *PGDI_TEB_BATCH64;

typedef struct _GDI_TEB_BATCH32
{
    ULONG Offset;
    EMULATOR_CAST(std::uint32_t, ULONG_PTR32) HDC;
    ULONG Buffer[GDI_BATCH_BUFFER_SIZE];
} GDI_TEB_BATCH32, *PGDI_TEB_BATCH32;

static_assert(sizeof(GDI_TEB_BATCH32) == 1248, "sizeof(GDI_TEB_BATCH32) is incorrect");

#ifndef OS_WINDOWS
typedef struct _GUID
{
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;

typedef struct _PROCESSOR_NUMBER
{
    WORD Group;
    BYTE Number;
    BYTE Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;

#endif

union TEB_CURRENT_IDEAL_PROCESSOR_UNION
{
    PROCESSOR_NUMBER CurrentIdealProcessor;
    ULONG IdealProcessorValue;

    struct
    {
        UCHAR ReservedPad0;
        UCHAR ReservedPad1;
        UCHAR ReservedPad2;
        UCHAR IdealProcessor;
    };
};

union TEB_CROSS_TEB_FLAGS_UNION
{
    USHORT CrossTebFlags;
    USHORT SpareCrossTebBits : 16;
};

constexpr auto THREAD_CREATE_FLAGS_CREATE_SUSPENDED = 0x1;
constexpr auto THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH = 0x2;
constexpr auto THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER = 0x4;
constexpr auto THREAD_CREATE_FLAGS_LOADER_WORKER = 0x10;
constexpr auto THREAD_CREATE_FLAGS_SKIP_LOADER_INIT = 0x20;
constexpr auto THREAD_CREATE_FLAGS_BYPASS_PROCESS_FREEZE = 0x40;

union TEB_SAME_TEB_FLAGS_UNION
{
    USHORT SameTebFlags;

    struct
    {
        USHORT SafeThunkCall : 1;
        USHORT InDebugPrint : 1;
        USHORT HasFiberData : 1;
        USHORT SkipThreadAttach : 1;
        USHORT WerInShipAssertCode : 1;
        USHORT RanProcessInit : 1;
        USHORT ClonedThread : 1;
        USHORT SuppressDebugMsg : 1;
        USHORT DisableUserStackWalk : 1;
        USHORT RtlExceptionAttached : 1;
        USHORT InitialThread : 1;
        USHORT SessionAware : 1;
        USHORT LoadOwner : 1;
        USHORT LoaderWorker : 1;
        USHORT SkipLoaderInit : 1;
        USHORT SkipFileAPIBrokering : 1;
    };
};

#ifndef OS_WINDOWS
using LCID = DWORD;
using LANGID = WORD;
#endif

typedef struct _TEB64
{
    EMU_NT_TIB64 NtTib;

    std::uint64_t EnvironmentPointer;
    CLIENT_ID64 ClientId;
    std::uint64_t ActiveRpcHandle;
    std::uint64_t ThreadLocalStoragePointer;
    EMULATOR_CAST(std::uint64_t, PPEB64) ProcessEnvironmentBlock;

    ULONG LastErrorValue;
    ULONG CountOfOwnedCriticalSections;
    std::uint64_t CsrClientThread;
    std::uint64_t Win32ThreadInfo;
    ARRAY_CONTAINER<ULONG, 26> User32Reserved;
    ARRAY_CONTAINER<ULONG, 5> UserReserved;
    std::uint64_t WOW32Reserved;
    LCID CurrentLocale;
    ULONG FpSoftwareStatusRegister;
    ARRAY_CONTAINER<std::uint64_t, 16> ReservedForDebuggerInstrumentation;
    ARRAY_CONTAINER<std::uint64_t, 25> SystemReserved1;
    std::uint64_t HeapFlsData;
    ARRAY_CONTAINER<std::uint64_t, 4> RngState;
    CHAR PlaceholderCompatibilityMode;
    BOOLEAN PlaceholderHydrationAlwaysExplicit;
    ARRAY_CONTAINER<CHAR, 10> PlaceholderReserved;

    ULONG ProxiedProcessId;
    ACTIVATION_CONTEXT_STACK64 ActivationStack;

    ARRAY_CONTAINER<UCHAR, 8> WorkingOnBehalfTicket;

    NTSTATUS ExceptionCode;

    EMULATOR_CAST(std::uint64_t, PACTIVATION_CONTEXT_STACK64) ActivationContextStackPointer;
    std::uint64_t InstrumentationCallbackSp;
    std::uint64_t InstrumentationCallbackPreviousPc;
    std::uint64_t InstrumentationCallbackPreviousSp;
    ULONG TxFsContext;
    BOOLEAN InstrumentationCallbackDisabled;
    BOOLEAN UnalignedLoadStoreExceptions;
    GDI_TEB_BATCH64 GdiTebBatch;
    CLIENT_ID64 RealClientId;
    EmulatorTraits<Emu64>::HANDLE GdiCachedProcessHandle;
    ULONG GdiClientPID;
    ULONG GdiClientTID;
    std::uint64_t GdiThreadLocalInfo;
    ARRAY_CONTAINER<std::uint64_t, WIN32_CLIENT_INFO_LENGTH> Win32ClientInfo;

    ARRAY_CONTAINER<std::uint64_t, 233> glDispatchTable;
    ARRAY_CONTAINER<std::uint64_t, 29> glReserved1;
    std::uint64_t glReserved2;
    std::uint64_t glSectionInfo;
    std::uint64_t glSection;
    std::uint64_t glTable;
    std::uint64_t glCurrentRC;
    std::uint64_t glContext;

    NTSTATUS LastStatusValue;

    UNICODE_STRING<EmulatorTraits<Emu64>> StaticUnicodeString;
    ARRAY_CONTAINER<char16_t, STATIC_UNICODE_BUFFER_LENGTH> StaticUnicodeBuffer;

    std::uint64_t DeallocationStack;

    ARRAY_CONTAINER<std::uint64_t, TLS_MINIMUM_AVAILABLE> TlsSlots;
    LIST_ENTRY64 TlsLinks;

    std::uint64_t Vdm;
    std::uint64_t ReservedForNtRpc;
    ARRAY_CONTAINER<std::uint64_t, 2> DbgSsReserved;

    ULONG HardErrorMode;
    ARRAY_CONTAINER<std::uint64_t, 11> Instrumentation;
    GUID ActivityId;

    std::uint64_t SubProcessTag;
    std::uint64_t PerflibData;
    std::uint64_t EtwTraceData;
    std::uint64_t WinSockData;
    ULONG GdiBatchCount;

    TEB_CURRENT_IDEAL_PROCESSOR_UNION CurrentIdealProcessor;

    ULONG GuaranteedStackBytes;
    std::uint64_t ReservedForPerf;
    std::uint64_t ReservedForOle; // tagSOleTlsData
    ULONG WaitingOnLoaderLock;
    std::uint64_t SavedPriorityState;
    std::uint64_t ReservedForCodeCoverage;
    std::uint64_t ThreadPoolData;
    std::uint64_t TlsExpansionSlots;
    std::uint64_t ChpeV2CpuAreaInfo; // CHPEV2_CPUAREA_INFO // previously DeallocationBStore
    std::uint64_t Unused;            // previously BStoreLimit
    ULONG MuiGeneration;
    ULONG IsImpersonating;
    std::uint64_t NlsCache;
    std::uint64_t pShimData;
    ULONG HeapData;
    EmulatorTraits<Emu64>::HANDLE CurrentTransactionHandle;
    EMULATOR_CAST(std::uint64_t, PTEB_ACTIVE_FRAME) ActiveFrame;
    std::uint64_t FlsData;

    std::uint64_t PreferredLanguages;
    std::uint64_t UserPrefLanguages;
    std::uint64_t MergedPrefLanguages;
    ULONG MuiImpersonation;

    TEB_CROSS_TEB_FLAGS_UNION CrossTebFlags;
    TEB_SAME_TEB_FLAGS_UNION SameTebFlags;

    std::uint64_t TxnScopeEnterCallback;
    std::uint64_t TxnScopeExitCallback;
    std::uint64_t TxnScopeContext;
    ULONG LockCount;
    LONG WowTebOffset;
    std::uint64_t ResourceRetValue;
    std::uint64_t ReservedForWdf;
    ULONGLONG ReservedForCrt;
    GUID EffectiveContainerId;
    ULONGLONG LastSleepCounter; // Win11
    ULONG SpinCallCount;
    ULONGLONG ExtendedFeatureDisableMask;
    std::uint64_t SchedulerSharedDataSlot; // 24H2
    std::uint64_t HeapWalkContext;
    EMU_GROUP_AFFINITY64 PrimaryGroupAffinity;
    ARRAY_CONTAINER<ULONG, 2> Rcu;
} TEB64, *PTEB64;

static_assert(sizeof(TEB64) == 0x1878);

#if defined(OS_WINDOWS) && defined(_WIN64)
inline TEB64* NtCurrentTeb64()
{
    return reinterpret_cast<TEB64*>(__readgsqword(FIELD_OFFSET(EMU_NT_TIB64, Self)));
}
#endif

typedef struct _TEB32
{
    EMU_NT_TIB32 NtTib;

    std::uint32_t EnvironmentPointer;
    CLIENT_ID32 ClientId;
    std::uint32_t ActiveRpcHandle;
    std::uint32_t ThreadLocalStoragePointer;
    EMULATOR_CAST(std::uint32_t, PPEB32) ProcessEnvironmentBlock;

    ULONG LastErrorValue;
    ULONG CountOfOwnedCriticalSections;
    std::uint32_t CsrClientThread;
    std::uint32_t Win32ThreadInfo;
    ULONG User32Reserved[26];
    ULONG UserReserved[5];
    std::uint32_t WOW32Reserved;
    LCID CurrentLocale;
    ULONG FpSoftwareStatusRegister;
    std::uint32_t ReservedForDebuggerInstrumentation[16];
    std::uint32_t SystemReserved1[26];

    CHAR PlaceholderCompatibilityMode;
    BOOLEAN PlaceholderHydrationAlwaysExplicit;
    CHAR PlaceholderReserved[10];

    ULONG ProxiedProcessId;
    ACTIVATION_CONTEXT_STACK32 ActivationStack;

    UCHAR WorkingOnBehalfTicket[8];
    NTSTATUS ExceptionCode;

    EMULATOR_CAST(std::uint32_t, PACTIVATION_CONTEXT_STACK32) ActivationContextStackPointer;
    EMULATOR_CAST(std::uint32_t, ULONG_PTR32) InstrumentationCallbackSp;
    EMULATOR_CAST(std::uint32_t, ULONG_PTR32) InstrumentationCallbackPreviousPc;
    EMULATOR_CAST(std::uint32_t, ULONG_PTR32) InstrumentationCallbackPreviousSp;

    BOOLEAN InstrumentationCallbackDisabled;
    UCHAR SpareBytes[23];
    ULONG TxFsContext;
    GDI_TEB_BATCH32 GdiTebBatch;
    CLIENT_ID32 RealClientId;
    EmulatorTraits<Emu32>::HANDLE GdiCachedProcessHandle;
    ULONG GdiClientPID;
    ULONG GdiClientTID;
    std::uint32_t GdiThreadLocalInfo;
    EMULATOR_CAST(std::uint32_t, ULONG_PTR32) Win32ClientInfo[WIN32_CLIENT_INFO_LENGTH];
    std::uint32_t glDispatchTable[233];
    EMULATOR_CAST(std::uint32_t, ULONG_PTR32) glReserved1[29];
    std::uint32_t glReserved2;
    std::uint32_t glSectionInfo;
    std::uint32_t glSection;
    std::uint32_t glTable;
    std::uint32_t glCurrentRC;
    std::uint32_t glContext;

    NTSTATUS LastStatusValue;
    UNICODE_STRING<EmulatorTraits<Emu32>> StaticUnicodeString;
    WCHAR StaticUnicodeBuffer[STATIC_UNICODE_BUFFER_LENGTH];

    std::uint32_t DeallocationStack;
    ARRAY_CONTAINER<std::uint32_t, TLS_MINIMUM_AVAILABLE> TlsSlots;
    LIST_ENTRY32 TlsLinks;

    std::uint32_t Vdm;
    std::uint32_t ReservedForNtRpc;
    std::uint32_t DbgSsReserved[2];

    ULONG HardErrorMode;
    std::uint32_t Instrumentation[9];
    GUID ActivityId;

    std::uint32_t SubProcessTag;
    std::uint32_t PerflibData;
    std::uint32_t EtwTraceData;
    std::uint32_t WinSockData;
    ULONG GdiBatchCount;

    union
    {
        EMULATOR_CAST(std::uint32_t, PROCESSOR_NUMBER) CurrentIdealProcessor;
        ULONG IdealProcessorValue;
        struct
        {
            UCHAR ReservedPad0;
            UCHAR ReservedPad1;
            UCHAR ReservedPad2;
            UCHAR IdealProcessor;
        };
    };

    ULONG GuaranteedStackBytes;
    EMULATOR_CAST(std::uint32_t, PVOID32) ReservedForPerf;
    EMULATOR_CAST(std::uint32_t, PVOID32) ReservedForOle; // tagSOleTlsData32
    ULONG WaitingOnLoaderLock;
    EMULATOR_CAST(std::uint32_t, PVOID32) SavedPriorityState;
    EMULATOR_CAST(std::uint32_t, ULONG_PTR32) ReservedForCodeCoverage;
    std::uint32_t ThreadPoolData;
    EMULATOR_CAST(std::uint32_t, PVOID32*) TlsExpansionSlots;
    ULONG MuiGeneration;
    ULONG IsImpersonating;
    std::uint32_t NlsCache;
    std::uint32_t pShimData;
    ULONG HeapData;
    EMULATOR_CAST(std::uint32_t, HANDLE32) CurrentTransactionHandle;
    EMULATOR_CAST(std::uint32_t, PTEB_ACTIVE_FRAME32) ActiveFrame;
    std::uint32_t FlsData;

    std::uint32_t PreferredLanguages;
    std::uint32_t UserPrefLanguages;
    std::uint32_t MergedPrefLanguages;
    ULONG MuiImpersonation;

    union
    {
        USHORT CrossTebFlags;
        USHORT SpareCrossTebBits : 16;
    };
    union
    {
        USHORT SameTebFlags;
        struct
        {
            USHORT SafeThunkCall : 1;
            USHORT InDebugPrint : 1;
            USHORT HasFiberData : 1;
            USHORT SkipThreadAttach : 1;
            USHORT WerInShipAssertCode : 1;
            USHORT RanProcessInit : 1;
            USHORT ClonedThread : 1;
            USHORT SuppressDebugMsg : 1;
            USHORT DisableUserStackWalk : 1;
            USHORT RtlExceptionAttached : 1;
            USHORT InitialThread : 1;
            USHORT SessionAware : 1;
            USHORT LoadOwner : 1;
            USHORT LoaderWorker : 1;
            USHORT SkipLoaderInit : 1;
            USHORT SkipFileAPIBrokering : 1;
        };
    };

    std::uint32_t TxnScopeEnterCallback;
    std::uint32_t TxnScopeExitCallback;
    std::uint32_t TxnScopeContext;
    ULONG LockCount;
    LONG WowTebOffset;
    std::uint32_t ResourceRetValue;
    std::uint32_t ReservedForWdf;
    ULONGLONG ReservedForCrt;
    GUID EffectiveContainerId;
    ULONGLONG LastSleepCounter; // Win11
    ULONG SpinCallCount;
    ULONGLONG ExtendedFeatureDisableMask;
} TEB32, *PTEB32;

static_assert(sizeof(TEB32) == 4120, "sizeof(TEB32) is incorrect");

#pragma pack(push, 4)
typedef struct _KSYSTEM_TIME
{
    ULONG LowPart;
    LONG High1Time;
    LONG High2Time;
} KSYSTEM_TIME, *PKSYSTEM_TIME;
#pragma pack(pop)

typedef enum _NT_PRODUCT_TYPE
{
    NtProductWinNt = 1,
    NtProductLanManNt,
    NtProductServer
} NT_PRODUCT_TYPE, *PNT_PRODUCT_TYPE;

typedef enum _ALTERNATIVE_ARCHITECTURE_TYPE
{
    StandardDesign,
    NEC98x86,
    EndAlternatives
} ALTERNATIVE_ARCHITECTURE_TYPE;

union KUSD_SHARED_DATA_FLAGS_UNION
{
    ULONG SharedDataFlags;

    struct
    {
        //
        // The following bit fields are for the debugger only. Do not use.
        // Use the bit definitions instead.
        //

        ULONG DbgErrorPortPresent : 1;
        ULONG DbgElevationEnabled : 1;
        ULONG DbgVirtEnabled : 1;
        ULONG DbgInstallerDetectEnabled : 1;
        ULONG DbgLkgEnabled : 1;
        ULONG DbgDynProcessorEnabled : 1;
        ULONG DbgConsoleBrokerEnabled : 1;
        ULONG DbgSecureBootEnabled : 1;
        ULONG DbgMultiSessionSku : 1;
        ULONG DbgMultiUsersInSessionSku : 1;
        ULONG DbgStateSeparationEnabled : 1;
        ULONG DbgSplitTokenEnabled : 1;
        ULONG DbgShadowAdminEnabled : 1;
        ULONG SpareBits : 19;
    };
};

union KUSD_TICK_COUNT_UNION
{
    volatile KSYSTEM_TIME TickCount;
    volatile std::uint64_t TickCountQuad;

    struct
    {
        ULONG ReservedTickCountOverlay[3];
        ULONG TickCountPad[1];
    };
};

union KUSD_VIRTUALIZATION_FLAGS_UNION
{
    UCHAR VirtualizationFlags;
};

union KUSD_MITIGATION_POLICIES_UNION
{
    UCHAR MitigationPolicies;

    struct
    {
        UCHAR NXSupportPolicy : 2;
        UCHAR SEHValidationPolicy : 2;
        UCHAR CurDirDevicesSkippedForDlls : 2;
        UCHAR Reserved : 2;
    };
};

union KUSD_QPC_DATA_UNION
{
    USHORT QpcData;

    struct
    {
        volatile UCHAR QpcBypassEnabled;
        UCHAR QpcReserved;
    };
};

#ifndef OS_WINDOWS
#define MAXIMUM_XSTATE_FEATURES 64

typedef struct _XSTATE_FEATURE
{
    ULONG Offset;
    ULONG Size;
} XSTATE_FEATURE;

typedef struct _XSTATE_CONFIGURATION
{
    std::uint64_t EnabledFeatures;
    std::uint64_t EnabledVolatileFeatures;
    ULONG Size;
    union
    {
        ULONG ControlFlags;
        struct
        {
            ULONG OptimizedSave : 1;
            ULONG CompactionEnabled : 1;
            ULONG Reserved1 : 30;
        };
    };
    XSTATE_FEATURE Features[MAXIMUM_XSTATE_FEATURES];
    std::uint64_t EnabledSupervisorFeatures;
    std::uint64_t AlignedFeatures;
    std::uint64_t AllFeatureSize;
    ULONG AllFeatures[MAXIMUM_XSTATE_FEATURES];
} XSTATE_CONFIGURATION, *PXSTATE_CONFIGURATION;

#endif

#define ORIGINALLY_VOLATILE /*volatile*/

typedef struct _KUSER_SHARED_DATA64
{
    ULONG TickCountLowDeprecated;
    ULONG TickCountMultiplier;
    ORIGINALLY_VOLATILE KSYSTEM_TIME InterruptTime;
    ORIGINALLY_VOLATILE KSYSTEM_TIME SystemTime;
    ORIGINALLY_VOLATILE KSYSTEM_TIME TimeZoneBias;
    USHORT ImageNumberLow;
    USHORT ImageNumberHigh;
    ARRAY_CONTAINER<char16_t, 260> NtSystemRoot;
    ULONG MaxStackTraceDepth;
    ULONG CryptoExponent;
    ULONG TimeZoneId;
    ULONG LargePageMinimum;
    ULONG AitSamplingValue;
    ULONG AppCompatFlag;
    ULONGLONG RNGSeedVersion;
    ULONG GlobalValidationRunlevel;
    ORIGINALLY_VOLATILE LONG TimeZoneBiasStamp;
    ULONG NtBuildNumber;
    NT_PRODUCT_TYPE NtProductType;
    BOOLEAN ProductTypeIsValid;
    BOOLEAN Reserved0;
    USHORT NativeProcessorArchitecture;
    ULONG NtMajorVersion;
    ULONG NtMinorVersion;
    ARRAY_CONTAINER<BOOLEAN, PROCESSOR_FEATURE_MAX> ProcessorFeatures;
    ULONG Reserved1;
    ULONG Reserved3;
    ORIGINALLY_VOLATILE ULONG TimeSlip;
    ALTERNATIVE_ARCHITECTURE_TYPE AlternativeArchitecture;
    ULONG BootId;
    LARGE_INTEGER SystemExpirationDate;
    ULONG SuiteMask;
    BOOLEAN KdDebuggerEnabled;
    KUSD_MITIGATION_POLICIES_UNION MitigationPolicies;
    USHORT CyclesPerYield;
    ORIGINALLY_VOLATILE ULONG ActiveConsoleId;
    ORIGINALLY_VOLATILE ULONG DismountCount;
    ULONG ComPlusPackage;
    ULONG LastSystemRITEventTickCount;
    ULONG NumberOfPhysicalPages;
    BOOLEAN SafeBootMode;
    KUSD_VIRTUALIZATION_FLAGS_UNION VirtualizationFlags;
    ARRAY_CONTAINER<UCHAR, 2> Reserved12;
    KUSD_SHARED_DATA_FLAGS_UNION SharedDataFlags;
    ULONG DataFlagsPad;
    ULONGLONG TestRetInstruction;
    LONGLONG QpcFrequency;
    ULONG SystemCall;
    ULONG Reserved2;
    ULONGLONG FullNumberOfPhysicalPages;
    ULONGLONG SystemCallPad;
    KUSD_TICK_COUNT_UNION TickCount;
    ULONG Cookie;
    ULONG CookiePad;
    LONGLONG ConsoleSessionForegroundProcessId;
    ULONGLONG TimeUpdateLock;
    ULONGLONG BaselineSystemTimeQpc;
    ULONGLONG BaselineInterruptTimeQpc;
    ULONGLONG QpcSystemTimeIncrement;
    ULONGLONG QpcInterruptTimeIncrement;
    UCHAR QpcSystemTimeIncrementShift;
    UCHAR QpcInterruptTimeIncrementShift;
    USHORT UnparkedProcessorCount;
    ARRAY_CONTAINER<ULONG, 4> EnclaveFeatureMask;
    ULONG TelemetryCoverageRound;
    ARRAY_CONTAINER<USHORT, 16> UserModeGlobalLogger;
    ULONG ImageFileExecutionOptions;
    ULONG LangGenerationCount;
    ULONGLONG Reserved4;
    ORIGINALLY_VOLATILE ULONGLONG InterruptTimeBias;
    ORIGINALLY_VOLATILE ULONGLONG QpcBias;
    ULONG ActiveProcessorCount;
    ORIGINALLY_VOLATILE UCHAR ActiveGroupCount;
    UCHAR Reserved9;
    KUSD_QPC_DATA_UNION QpcData;
    LARGE_INTEGER TimeZoneBiasEffectiveStart;
    LARGE_INTEGER TimeZoneBiasEffectiveEnd;
    XSTATE_CONFIGURATION XState;
    KSYSTEM_TIME FeatureConfigurationChangeStamp;
    ULONG Spare;
    std::uint64_t UserPointerAuthMask;
    ARRAY_CONTAINER<ULONG, 210> Reserved10;
} KUSER_SHARED_DATA64, *PKUSER_SHARED_DATA64;

typedef struct _API_SET_NAMESPACE_ENTRY
{
    ULONG Flags;
    ULONG NameOffset;
    ULONG NameLength;
    ULONG HashedLength;
    ULONG ValueOffset;
    ULONG ValueCount;
} API_SET_NAMESPACE_ENTRY, *PAPI_SET_NAMESPACE_ENTRY;

typedef struct _API_SET_HASH_ENTRY
{
    ULONG Hash;
    ULONG Index;
} API_SET_HASH_ENTRY, *PAPI_SET_HASH_ENTRY;

typedef struct _API_SET_VALUE_ENTRY
{
    ULONG Flags;
    ULONG NameOffset;
    ULONG NameLength;
    ULONG ValueOffset;
    ULONG ValueLength;
} API_SET_VALUE_ENTRY, *PAPI_SET_VALUE_ENTRY;

template <typename Traits>
struct PS_ATTRIBUTE
{
    typename Traits::ULONG_PTR Attribute;
    typename Traits::SIZE_T Size;

    union
    {
        typename Traits::ULONG_PTR Value;
        typename Traits::PVOID ValuePtr;
    };

    EMULATOR_CAST(uint64_t, typename Traits::SIZE_T*) ReturnLength;
};

template <typename Traits>
struct PS_ATTRIBUTE_LIST
{
    typename Traits::SIZE_T TotalLength;
    PS_ATTRIBUTE<Traits> Attributes[1];
};

typedef struct _SYSTEM_TIMEOFDAY_INFORMATION64
{
    LARGE_INTEGER BootTime;
    LARGE_INTEGER CurrentTime;
    LARGE_INTEGER TimeZoneBias;
    ULONG TimeZoneId;
    ULONG Reserved;
    ULONGLONG BootTimeBias;
    ULONGLONG SleepTimeBias;
} SYSTEM_TIMEOFDAY_INFORMATION64, *PSYSTEM_TIMEOFDAY_INFORMATION64;

typedef struct _SYSTEMTIME64
{
    WORD wYear;
    WORD wMonth;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
    WORD wDayOfWeek;
} SYSTEMTIME64, *PSYSTEMTIME64, *LPSYSTEMTIME64;

typedef struct _SYSTEM_TIMEZONE_INFORMATION
{
    LONG Bias;
    ARRAY_CONTAINER<char16_t, 32> StandardName;
    SYSTEMTIME64 StandardDate;
    LONG StandardBias;
    ARRAY_CONTAINER<char16_t, 32> DaylightName;
    SYSTEMTIME64 DaylightDate;
    LONG DaylightBias;
} SYSTEM_TIMEZONE_INFORMATION, *PSYSTEM_TIMEZONE_INFORMATION;

typedef struct _SYSTEM_DYNAMIC_TIMEZONE_INFORMATION
{
    LONG Bias;
    ARRAY_CONTAINER<char16_t, 32> StandardName;
    SYSTEMTIME64 StandardDate;
    LONG StandardBias;
    ARRAY_CONTAINER<char16_t, 32> DaylightName;
    SYSTEMTIME64 DaylightDate;
    LONG DaylightBias;
    ARRAY_CONTAINER<char16_t, 128> TimeZoneKeyName;
    BOOLEAN DynamicDaylightTimeDisabled;
} SYSTEM_DYNAMIC_TIMEZONE_INFORMATION, *PSYSTEM_DYNAMIC_TIMEZONE_INFORMATION;

// Memory address requirements structure
typedef struct _MEM_ADDRESS_REQUIREMENTS64
{
    EMULATOR_CAST(std::uint64_t, PVOID) LowestStartingAddress;
    EMULATOR_CAST(std::uint64_t, PVOID) HighestEndingAddress;
    EMULATOR_CAST(std::uint64_t, SIZE_T) Alignment;
} MEM_ADDRESS_REQUIREMENTS64, *PMEM_ADDRESS_REQUIREMENTS64;

// Extended memory parameter structure
typedef struct _MEM_EXTENDED_PARAMETER64
{
    struct
    {
        ULONG64 Type : 8; // MEM_EXTENDED_PARAMETER_TYPE
        ULONG64 Reserved : 56;
    };

    union
    {
        ULONG64 ULong64;
        EMULATOR_CAST(std::uint64_t, PVOID) Pointer;
        EMULATOR_CAST(std::uint64_t, SIZE_T) Size;
        EMULATOR_CAST(std::uint64_t, HANDLE) Handle;
        ULONG ULong;
    };
} MEM_EXTENDED_PARAMETER64, *PMEM_EXTENDED_PARAMETER64;

typedef struct _PROCESS_BASIC_INFORMATION64
{
    NTSTATUS ExitStatus;
    EMULATOR_CAST(uint64_t, PPEB64) PebBaseAddress;
    EMULATOR_CAST(std::uint64_t, KAFFINITY) AffinityMask;
    EMULATOR_CAST(std::uint32_t, KPRIORITY) BasePriority;
    EMULATOR_CAST(std::uint64_t, HANDLE) UniqueProcessId;
    EMULATOR_CAST(std::uint64_t, HANDLE) InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION64, *PPROCESS_BASIC_INFORMATION64;

typedef struct _PROCESS_EXTENDED_BASIC_INFORMATION
{
    EMULATOR_CAST(std::uint64_t, SIZE_T) Size; // Ignored as input, written with structure size on output
    PROCESS_BASIC_INFORMATION64 BasicInfo;
    union
    {
        ULONG Flags;
        struct
        {
            ULONG IsProtectedProcess : 1;
            ULONG IsWow64Process : 1;
            ULONG IsProcessDeleting : 1;
            ULONG IsCrossSessionCreate : 1;
            ULONG IsFrozen : 1;
            ULONG IsBackground : 1;
            ULONG IsStronglyNamed : 1;
            ULONG IsSecureProcess : 1;
            ULONG IsSubsystemProcess : 1;
            ULONG SpareBits : 23;
        };
    };
} PROCESS_EXTENDED_BASIC_INFORMATION, *PPROCESS_EXTENDED_BASIC_INFORMATION;

typedef struct _KERNEL_USER_TIMES
{
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
} KERNEL_USER_TIMES, *PKERNEL_USER_TIMES;

struct THREAD_TLS_INFORMATION
{
    ULONG Flags;
    uint32_t _Padding;

    union
    {
        EmulatorTraits<Emu64>::PVOID NewTlsData;
        EmulatorTraits<Emu64>::PVOID OldTlsData;
    };

    uint64_t ThreadId;
};

static_assert(sizeof(THREAD_TLS_INFORMATION) == 0x18);

enum PROCESS_TLS_INFORMATION_TYPE
{
    ProcessTlsReplaceIndex,
    ProcessTlsReplaceVector,
    MaxProcessTlsOperation
};

struct PROCESS_TLS_INFORMATION
{
    ULONG Flags;
    PROCESS_TLS_INFORMATION_TYPE OperationType;
    ULONG ThreadDataCount;

    union
    {
        ULONG TlsIndex;
        ULONG PreviousCount;
    };

    THREAD_TLS_INFORMATION ThreadData[1];
};

static_assert(sizeof(PROCESS_TLS_INFORMATION) == 0x28);

constexpr auto PROCESS_TLS_FLAG_USE_TEB32 = 0x1u;
constexpr auto PROCESS_TLS_FLAG_VALID_MASK = PROCESS_TLS_FLAG_USE_TEB32;

struct EMU_GENERIC_MAPPING
{
    ACCESS_MASK GenericRead;
    ACCESS_MASK GenericWrite;
    ACCESS_MASK GenericExecute;
    ACCESS_MASK GenericAll;
};

struct OBJECT_TYPES_INFORMATION
{
    ULONG NumberOfTypes;
};

struct OBJECT_TYPE_INFORMATION
{
    STRING64 TypeName;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG TotalPagedPoolUsage;
    ULONG TotalNonPagedPoolUsage;
    ULONG TotalNamePoolUsage;
    ULONG TotalHandleTableUsage;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG HighWaterPagedPoolUsage;
    ULONG HighWaterNonPagedPoolUsage;
    ULONG HighWaterNamePoolUsage;
    ULONG HighWaterHandleTableUsage;
    ULONG InvalidAttributes;
    EMU_GENERIC_MAPPING GenericMapping;
    ULONG ValidAccessMask;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    UCHAR TypeIndex; // since WINBLUE
    CHAR ReservedByte;
    ULONG PoolType;
    ULONG DefaultPagedPoolCharge;
    ULONG DefaultNonPagedPoolCharge;
};

// WIN8 to REDSTONE
typedef struct _PS_MITIGATION_OPTIONS_MAP_V1
{
    ULONG64 Map[1];
} PS_MITIGATION_OPTIONS_MAP_V1, *PPS_MITIGATION_OPTIONS_MAP_V1;

// private // REDSTONE2 to 19H2
typedef struct _PS_MITIGATION_OPTIONS_MAP_V2
{
    ULONG64 Map[2];
} PS_MITIGATION_OPTIONS_MAP_V2, *PPS_MITIGATION_OPTIONS_MAP_V2;

// private // since 20H1
typedef struct _PS_MITIGATION_OPTIONS_MAP_V3
{
    ULONG64 Map[3];
} PS_MITIGATION_OPTIONS_MAP_V3, *PPS_MITIGATION_OPTIONS_MAP_V3;

typedef PS_MITIGATION_OPTIONS_MAP_V3 PS_MITIGATION_OPTIONS_MAP, *PPS_MITIGATION_OPTIONS_MAP;

// private // REDSTONE3 to 19H2
typedef struct _PS_MITIGATION_AUDIT_OPTIONS_MAP_V2
{
    ULONG64 Map[2];
} PS_MITIGATION_AUDIT_OPTIONS_MAP_V2, *PPS_MITIGATION_AUDIT_OPTIONS_MAP_V2;

// private // since 20H1
typedef struct _PS_MITIGATION_AUDIT_OPTIONS_MAP_V3
{
    ULONG64 Map[3];
} PS_MITIGATION_AUDIT_OPTIONS_MAP_V3, *PPS_MITIGATION_AUDIT_OPTIONS_MAP_V3, PS_MITIGATION_AUDIT_OPTIONS_MAP,
    *PPS_MITIGATION_AUDIT_OPTIONS_MAP;

// private // VISTA to WIN7 SP1
typedef enum class _WOW64_SHARED_INFORMATION_V1
{
    SharedNtdll32LdrInitializeThunk = 0,
    SharedNtdll32KiUserExceptionDispatcher = 1,
    SharedNtdll32KiUserApcDispatcher = 2,
    SharedNtdll32KiUserCallbackDispatcher = 3,
    SharedNtdll32LdrHotPatchRoutine = 4,
    SharedNtdll32ExpInterlockedPopEntrySListFault = 5,
    SharedNtdll32ExpInterlockedPopEntrySListResume = 6,
    SharedNtdll32ExpInterlockedPopEntrySListEnd = 7,
    SharedNtdll32RtlUserThreadStart = 8,
    SharedNtdll32pQueryProcessDebugInformationRemote = 9,
    SharedNtdll32EtwpNotificationThread = 10,
    SharedNtdll32BaseAddress = 11,
    Wow64SharedPageEntriesCount = 12
} WOW64_SHARED_INFORMATION_V1;

// private // WIN8
typedef enum class _WOW64_SHARED_INFORMATION_V2
{
    SharedNtdll32LdrInitializeThunk = 0,
    SharedNtdll32KiUserExceptionDispatcher = 1,
    SharedNtdll32KiUserApcDispatcher = 2,
    SharedNtdll32KiUserCallbackDispatcher = 3,
    SharedNtdll32LdrHotPatchRoutine = 4,
    SharedNtdll32ExpInterlockedPopEntrySListFault = 5,
    SharedNtdll32ExpInterlockedPopEntrySListResume = 6,
    SharedNtdll32ExpInterlockedPopEntrySListEnd = 7,
    SharedNtdll32RtlUserThreadStart = 8,
    SharedNtdll32pQueryProcessDebugInformationRemote = 9,
    SharedNtdll32EtwpNotificationThread = 10,
    SharedNtdll32BaseAddress = 11,
    SharedNtdll32RtlpWnfNotificationThread = 12,
    SharedNtdll32LdrSystemDllInitBlock = 13,
    Wow64SharedPageEntriesCount = 14
} WOW64_SHARED_INFORMATION_V2;

// private // WIN8.1 to THRESHOLD 1
typedef enum class _WOW64_SHARED_INFORMATION_V3
{
    SharedNtdll32LdrInitializeThunk = 0,
    SharedNtdll32KiUserExceptionDispatcher = 1,
    SharedNtdll32KiUserApcDispatcher = 2,
    SharedNtdll32KiUserCallbackDispatcher = 3,
    SharedNtdll32ExpInterlockedPopEntrySListFault = 4,
    SharedNtdll32ExpInterlockedPopEntrySListResume = 5,
    SharedNtdll32ExpInterlockedPopEntrySListEnd = 6,
    SharedNtdll32RtlUserThreadStart = 7,
    SharedNtdll32pQueryProcessDebugInformationRemote = 8,
    SharedNtdll32BaseAddress = 9,
    SharedNtdll32LdrSystemDllInitBlock = 10,
    Wow64SharedPageEntriesCount = 11
} WOW64_SHARED_INFORMATION_V3;

// private // THRESHOLD 2 to REDSTONE 2
typedef enum class _WOW64_SHARED_INFORMATION_V4
{
    SharedNtdll32LdrInitializeThunk = 0,
    SharedNtdll32KiUserExceptionDispatcher = 1,
    SharedNtdll32KiUserApcDispatcher = 2,
    SharedNtdll32KiUserCallbackDispatcher = 3,
    SharedNtdll32RtlUserThreadStart = 4,
    SharedNtdll32pQueryProcessDebugInformationRemote = 5,
    SharedNtdll32BaseAddress = 6,
    SharedNtdll32LdrSystemDllInitBlock = 7,
    Wow64SharedPageEntriesCount = 8
} WOW64_SHARED_INFORMATION_V4;

// private // REDSTONE 3 to 24H2
typedef enum class _WOW64_SHARED_INFORMATION_V5
{
    SharedNtdll32LdrInitializeThunk = 0,
    SharedNtdll32KiUserExceptionDispatcher = 1,
    SharedNtdll32KiUserApcDispatcher = 2,
    SharedNtdll32KiUserCallbackDispatcher = 3,
    SharedNtdll32RtlUserThreadStart = 4,
    SharedNtdll32pQueryProcessDebugInformationRemote = 5,
    SharedNtdll32BaseAddress = 6,
    SharedNtdll32LdrSystemDllInitBlock = 7,
    SharedNtdll32RtlpFreezeTimeBias = 8,
    Wow64SharedPageEntriesCount = 9
} WOW64_SHARED_INFORMATION_V5;

// private // WIN8 to REDSTONE
typedef struct _PS_SYSTEM_DLL_INIT_BLOCK_V1
{
    ULONG Size;
    ULONG SystemDllWowRelocation;
    ULONG64 SystemDllNativeRelocation;
    ULONG Wow64SharedInformation[16]; // use WOW64_SHARED_INFORMATION as index
    ULONG RngData;
    union
    {
        ULONG Flags;
        struct
        {
            ULONG CfgOverride : 1; // since REDSTONE
            ULONG Reserved : 31;
        };
    };
    ULONG64 MitigationOptions;
    ULONG64 CfgBitMap; // since WINBLUE
    ULONG64 CfgBitMapSize;
    ULONG64 Wow64CfgBitMap; // since THRESHOLD
    ULONG64 Wow64CfgBitMapSize;
} PS_SYSTEM_DLL_INIT_BLOCK_V1, *PPS_SYSTEM_DLL_INIT_BLOCK_V1;

// RS2 - 19H2
typedef struct _PS_SYSTEM_DLL_INIT_BLOCK_V2
{
    ULONG Size;
    ULONG64 SystemDllWowRelocation;
    ULONG64 SystemDllNativeRelocation;
    ULONG64 Wow64SharedInformation[16]; // use WOW64_SHARED_INFORMATION as index
    ULONG RngData;
    union
    {
        ULONG Flags;
        struct
        {
            ULONG CfgOverride : 1;
            ULONG Reserved : 31;
        };
    };
    PS_MITIGATION_OPTIONS_MAP_V2 MitigationOptionsMap;
    ULONG64 CfgBitMap;
    ULONG64 CfgBitMapSize;
    ULONG64 Wow64CfgBitMap;
    ULONG64 Wow64CfgBitMapSize;
    PS_MITIGATION_AUDIT_OPTIONS_MAP_V2 MitigationAuditOptionsMap; // since REDSTONE3
} PS_SYSTEM_DLL_INIT_BLOCK_V2, *PPS_SYSTEM_DLL_INIT_BLOCK_V2;

// private // since 20H1
typedef struct _PS_SYSTEM_DLL_INIT_BLOCK_V3
{
    ULONG Size;
    ULONG64 SystemDllWowRelocation; // effectively since WIN8
    ULONG64 SystemDllNativeRelocation;
    ULONG64 Wow64SharedInformation[16]; // use WOW64_SHARED_INFORMATION_V5 as index
    ULONG RngData;
    union
    {
        ULONG Flags;
        struct
        {
            ULONG CfgOverride : 1; // effectively since REDSTONE
            ULONG Reserved : 31;
        };
    };
    PS_MITIGATION_OPTIONS_MAP_V3 MitigationOptionsMap;
    ULONG64 CfgBitMap; // effectively since WINBLUE
    ULONG64 CfgBitMapSize;
    ULONG64 Wow64CfgBitMap; // effectively since THRESHOLD
    ULONG64 Wow64CfgBitMapSize;
    PS_MITIGATION_AUDIT_OPTIONS_MAP_V3 MitigationAuditOptionsMap; // effectively since REDSTONE3
    ULONG64 ScpCfgCheckFunction;                                  // since 24H2
    ULONG64 ScpCfgCheckESFunction;
    ULONG64 ScpCfgDispatchFunction;
    ULONG64 ScpCfgDispatchESFunction;
    ULONG64 ScpArm64EcCallCheck;
    ULONG64 ScpArm64EcCfgCheckFunction;
    ULONG64 ScpArm64EcCfgCheckESFunction;
} PS_SYSTEM_DLL_INIT_BLOCK_V3, *PPS_SYSTEM_DLL_INIT_BLOCK_V3, PS_SYSTEM_DLL_INIT_BLOCK, *PPS_SYSTEM_DLL_INIT_BLOCK;

static_assert(sizeof(PS_SYSTEM_DLL_INIT_BLOCK_V3) == 0x128);

//
// Process mitigation policy information
//  NtSetInformationProcess using ProcessMitigationPolicy
//

typedef struct _PROCESS_MITIGATION_POLICY_RAW_DATA
{
    PROCESS_MITIGATION_POLICY Policy;
    ULONG Value;
} PROCESS_MITIGATION_POLICY_RAW_DATA, *PPROCESS_MITIGATION_POLICY_RAW_DATA;

static_assert(sizeof(PROCESS_MITIGATION_POLICY_RAW_DATA) == 0x8);

typedef struct _WOW64_CPURESERVED
{
    USHORT Flags;       // Initialised to 0x02 by ntdll.dll and periodically bitwise OR'd with WOW64_CPURESERVED_FLAG_RESET_STATE
    USHORT MachineType; // IMAGE_FILE_MACHINE_I386 (0x014C) for x86 architecture

    // Under IMAGE_FILE_MACHINE_I386 machine type, this is the WOW64_CONTEXT structure containing x86 architecture context
    WOW64_CONTEXT Context;

    // Padding to align WOW64 stack
    BYTE AlignmentPad[0x10];
} WOW64_CPURESERVED, *PWOW64_CPURESERVED;

static_assert(sizeof(WOW64_CPURESERVED) == 0x2E0);

#define WOW64_CPURESERVED_FLAG_RESET_STATE 1

typedef enum class _SECTION_INFORMATION_CLASS
{
    SectionBasicInformation = 0,
    SectionImageInformation = 1,
    SectionRelocationInformation = 2,
    SectionOriginalBaseInformation = 3,
    SectionInternalImageInformation = 4
} SECTION_INFORMATION_CLASS, *PSECTION_INFORMATION_CLASS;

typedef enum _WINDOWS_VERSION : uint32_t
{
    WINDOWS_10_1507 = 10240,
    WINDOWS_10_1511 = 10586,
    WINDOWS_10_1607 = 14393,
    WINDOWS_10_1703 = 15063,
    WINDOWS_10_1709 = 16299,
    WINDOWS_10_1803 = 17134,
    WINDOWS_10_1809 = 17763,
    WINDOWS_10_1903 = 18362,
    WINDOWS_10_1909 = 18363,
    WINDOWS_10_2004 = 19041,
    WINDOWS_10_20H2 = 19042,
    WINDOWS_10_21H1 = 19043,
    WINDOWS_10_21H2 = 19044,
    WINDOWS_10_22H2 = 19045,
    WINDOWS_11_21H2 = 22000,
    WINDOWS_11_22H2 = 22621,
    WINDOWS_11_23H2 = 22631,
    WINDOWS_11_24H2 = 26100,
} WINDOWS_VERSION, *PWINDOWS_VERSION;

constexpr uint64_t PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V1 = sizeof(PS_SYSTEM_DLL_INIT_BLOCK_V1);
constexpr uint64_t PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V2_1703 = offsetof(PS_SYSTEM_DLL_INIT_BLOCK_V2, MitigationAuditOptionsMap);
constexpr uint64_t PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V2 = sizeof(PS_SYSTEM_DLL_INIT_BLOCK_V2);
constexpr uint64_t PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V3_2004 = offsetof(PS_SYSTEM_DLL_INIT_BLOCK_V3, ScpCfgCheckFunction);
constexpr uint64_t PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V3 = sizeof(PS_SYSTEM_DLL_INIT_BLOCK_V3);

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
