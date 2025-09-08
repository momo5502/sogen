#pragma once

#include <cstdint>

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

#define PROCESSOR_FEATURE_MAX                                           64
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
    std::uint64_t FibreData;
    std::uint64_t ArbitraryUserPointer;
    EMULATOR_CAST(std::uint64_t, struct _EMU_NT_TIB64*) Self;
} EMU_NT_TIB64;

typedef EMU_NT_TIB64* PEMU_NT_TIB64;

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
    EMULATOR_CAST(std::int64_t, SIZE_T) HeapSegmentReserve;
    EMULATOR_CAST(std::int64_t, SIZE_T) HeapSegmentCommit;
    EMULATOR_CAST(std::int64_t, SIZE_T) HeapDeCommitTotalFreeThreshold;
    EMULATOR_CAST(std::int64_t, SIZE_T) HeapDeCommitFreeBlockThreshold;

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

typedef struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME64
{
    struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME* Previous;
    EMULATOR_CAST(std::uint64_t, ACTIVATION_CONTEXT) ActivationContext;
    ULONG Flags; // RTL_ACTIVATION_CONTEXT_STACK_FRAME_FLAG_*
} RTL_ACTIVATION_CONTEXT_STACK_FRAME64, *PRTL_ACTIVATION_CONTEXT_STACK_FRAME64;

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

typedef struct _PROCESS_BASIC_INFORMATION64
{
    NTSTATUS ExitStatus;
    EMULATOR_CAST(uint64_t, PPEB64) PebBaseAddress;
    EMULATOR_CAST(std::uint64_t, KAFFINITY) AffinityMask;
    EMULATOR_CAST(std::uint32_t, KPRIORITY) BasePriority;
    EMULATOR_CAST(std::uint64_t, HANDLE) UniqueProcessId;
    EMULATOR_CAST(std::uint64_t, HANDLE) InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION64, *PPROCESS_BASIC_INFORMATION64;

typedef struct _KERNEL_USER_TIMES
{
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
} KERNEL_USER_TIMES, *PKERNEL_USER_TIMES;

struct THREAD_TLS_INFO
{
    ULONG Flags;
    uint32_t _Padding;

    union
    {
        EmulatorTraits<Emu64>::PVOID TlsVector;
        EmulatorTraits<Emu64>::PVOID TlsModulePointer;
    };

    EMULATOR_CAST(std::uint64_t, ULONG_PTR) ThreadId;
};

static_assert(sizeof(THREAD_TLS_INFO) == 0x18);

typedef enum _PROCESS_TLS_INFORMATION_TYPE
{
    ProcessTlsReplaceIndex,
    ProcessTlsReplaceVector,
    MaxProcessTlsOperation
} PROCESS_TLS_INFORMATION_TYPE, *PPROCESS_TLS_INFORMATION_TYPE;

struct PROCESS_TLS_INFO
{
    ULONG Unknown;
    PROCESS_TLS_INFORMATION_TYPE TlsRequest;
    ULONG ThreadDataCount;

    union
    {
        ULONG TlsIndex;
        ULONG TlsVectorLength;
    };

    THREAD_TLS_INFO ThreadData[1];
};

static_assert(sizeof(PROCESS_TLS_INFO) - sizeof(THREAD_TLS_INFO) == 0x10);

struct EMU_GENERIC_MAPPING
{
    ACCESS_MASK GenericRead;
    ACCESS_MASK GenericWrite;
    ACCESS_MASK GenericExecute;
    ACCESS_MASK GenericAll;
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

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
