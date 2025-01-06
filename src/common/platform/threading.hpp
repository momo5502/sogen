#pragma once

typedef enum _THREADINFOCLASS
{
    ThreadBasicInformation,          // q: THREAD_BASIC_INFORMATION
    ThreadTimes,                     // q: KERNEL_USER_TIMES
    ThreadPriority,                  // s: KPRIORITY (requires SeIncreaseBasePriorityPrivilege)
    ThreadBasePriority,              // s: KPRIORITY
    ThreadAffinityMask,              // s: KAFFINITY
    ThreadImpersonationToken,        // s: HANDLE
    ThreadDescriptorTableEntry,      // q: DESCRIPTOR_TABLE_ENTRY (or WOW64_DESCRIPTOR_TABLE_ENTRY)
    ThreadEnableAlignmentFaultFixup, // s: BOOLEAN
    ThreadEventPair,
    ThreadQuerySetWin32StartAddress, // q: ULONG_PTR
    ThreadZeroTlsCell,               // s: ULONG // TlsIndex // 10
    ThreadPerformanceCount,          // q: LARGE_INTEGER
    ThreadAmILastThread,             // q: ULONG
    ThreadIdealProcessor,            // s: ULONG
    ThreadPriorityBoost,             // qs: ULONG
    ThreadSetTlsArrayAddress,        // s: ULONG_PTR // Obsolete
    ThreadIsIoPending,               // q: ULONG
    ThreadHideFromDebugger,          // q: BOOLEAN; s: void
    ThreadBreakOnTermination,        // qs: ULONG
    ThreadSwitchLegacyState,         // s: void // NtCurrentThread // NPX/FPU
    ThreadIsTerminated,              // q: ULONG // 20
    ThreadLastSystemCall,            // q: THREAD_LAST_SYSCALL_INFORMATION
    ThreadIoPriority,                // qs: IO_PRIORITY_HINT (requires SeIncreaseBasePriorityPrivilege)
    ThreadCycleTime,                 // q: THREAD_CYCLE_TIME_INFORMATION
    ThreadPagePriority,              // qs: PAGE_PRIORITY_INFORMATION
    ThreadActualBasePriority,        // s: LONG (requires SeIncreaseBasePriorityPrivilege)
    ThreadTebInformation,            // q: THREAD_TEB_INFORMATION (requires THREAD_GET_CONTEXT + THREAD_SET_CONTEXT)
    ThreadCSwitchMon,                // Obsolete
    ThreadCSwitchPmu,
    ThreadWow64Context,             // qs: WOW64_CONTEXT, ARM_NT_CONTEXT since 20H1
    ThreadGroupInformation,         // qs: GROUP_AFFINITY // 30
    ThreadUmsInformation,           // q: THREAD_UMS_INFORMATION // Obsolete
    ThreadCounterProfiling,         // q: BOOLEAN; s: THREAD_PROFILING_INFORMATION?
    ThreadIdealProcessorEx,         // qs: PROCESSOR_NUMBER; s: previous PROCESSOR_NUMBER on return
    ThreadCpuAccountingInformation, // q: BOOLEAN; s: HANDLE (NtOpenSession) // NtCurrentThread // since WIN8
    ThreadSuspendCount,             // q: ULONG // since WINBLUE
    ThreadHeterogeneousCpuPolicy,   // q: KHETERO_CPU_POLICY // since THRESHOLD
    ThreadContainerId,              // q: GUID
    ThreadNameInformation,          // qs: THREAD_NAME_INFORMATION
    ThreadSelectedCpuSets,
    ThreadSystemThreadInformation,        // q: SYSTEM_THREAD_INFORMATION // 40
    ThreadActualGroupAffinity,            // q: GROUP_AFFINITY // since THRESHOLD2
    ThreadDynamicCodePolicyInfo,          // q: ULONG; s: ULONG (NtCurrentThread)
    ThreadExplicitCaseSensitivity,        // qs: ULONG; s: 0 disables, otherwise enables
    ThreadWorkOnBehalfTicket,             // RTL_WORK_ON_BEHALF_TICKET_EX
    ThreadSubsystemInformation,           // q: SUBSYSTEM_INFORMATION_TYPE // since REDSTONE2
    ThreadDbgkWerReportActive,            // s: ULONG; s: 0 disables, otherwise enables
    ThreadAttachContainer,                // s: HANDLE (job object) // NtCurrentThread
    ThreadManageWritesToExecutableMemory, // MANAGE_WRITES_TO_EXECUTABLE_MEMORY // since REDSTONE3
    ThreadPowerThrottlingState,           // POWER_THROTTLING_THREAD_STATE // since REDSTONE3 (set), WIN11 22H2 (query)
    ThreadWorkloadClass,                  // THREAD_WORKLOAD_CLASS // since REDSTONE5 // 50
    ThreadCreateStateChange,              // since WIN11
    ThreadApplyStateChange,
    ThreadStrongerBadHandleChecks, // since 22H1
    ThreadEffectiveIoPriority,     // q: IO_PRIORITY_HINT
    ThreadEffectivePagePriority,   // q: ULONG
    ThreadUpdateLockOwnership,     // since 24H2
    ThreadSchedulerSharedDataSlot, // SCHEDULER_SHARED_DATA_SLOT_INFORMATION
    ThreadTebInformationAtomic,    // THREAD_TEB_INFORMATION
    ThreadIndexInformation,        // THREAD_INDEX_INFORMATION
    MaxThreadInfoClass
} THREADINFOCLASS;

template <typename Traits>
struct THREAD_NAME_INFORMATION
{
    UNICODE_STRING<Traits> ThreadName;
};

typedef struct _THREAD_BASIC_INFORMATION64
{
    NTSTATUS ExitStatus;
    PTEB64 TebBaseAddress;
    CLIENT_ID64 ClientId;
    EMULATOR_CAST(std::uint64_t, KAFFINITY) AffinityMask;
    EMULATOR_CAST(std::uint32_t, KPRIORITY) Priority;
    EMULATOR_CAST(std::uint32_t, KPRIORITY) BasePriority;
} THREAD_BASIC_INFORMATION64, *PTHREAD_BASIC_INFORMATION64;
