#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            NTSTATUS handle_system_memory_usage_information(const syscall_context& c, const uint32_t info_class,
                                                            const uint64_t system_information, const uint32_t system_information_length,
                                                            const emulator_object<uint32_t> return_length)
            {
                struct basic_memory_status_information
                {
                    uint64_t total_physical_bytes;
                    uint64_t available_physical_bytes;
                    uint64_t reserved0;
                    uint64_t committed_bytes;
                    uint64_t reserved1;
                    uint64_t commit_limit_bytes;
                    uint64_t reserved2;
                };

                if (info_class == SystemMemoryUsageInformation && system_information_length == sizeof(basic_memory_status_information))
                {
                    // Simulated physical memory size (~13 GB).
                    constexpr uint64_t total_physical_bytes = 0x00c9c7ffULL * 0x1000;

                    const basic_memory_status_information info{
                        .total_physical_bytes = total_physical_bytes,
                        .available_physical_bytes = total_physical_bytes / 2,
                        .reserved0 = 0,
                        .committed_bytes = total_physical_bytes / 4,
                        .reserved1 = 0,
                        .commit_limit_bytes = total_physical_bytes + total_physical_bytes / 2,
                        .reserved2 = 0,
                    };

                    c.emu.write_memory(system_information, &info, sizeof(info));
                    if (return_length)
                    {
                        return_length.write(sizeof(info));
                    }

                    return STATUS_SUCCESS;
                }

                using Traits = EmulatorTraits<Emu64>;
                using info_t = SYSTEM_MEMORY_USAGE_INFORMATION<Traits>;
                using usage_t = SYSTEM_MEMORY_USAGE<Traits>;

                constexpr auto header_size = static_cast<uint32_t>(offsetof(info_t, MemoryUsage));
                constexpr auto usage_name = std::to_array(u"System Memory");
                constexpr auto required_length = header_size + static_cast<uint32_t>(sizeof(usage_t) + sizeof(usage_name));

                if (return_length)
                {
                    return_length.write(required_length);
                }

                if (system_information_length < required_length)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const info_t header{.Reserved = 0, .EndOfData = system_information + header_size + sizeof(usage_t), .MemoryUsage = {}};
                c.emu.write_memory(system_information, &header, header_size);

                const USHORT base_valid = info_class == SystemFullMemoryInformation ? 0x2000 : 0x1000;
                const usage_t usage{
                    .Name = system_information + header_size + sizeof(usage_t),
                    .Valid = base_valid,
                    .Standby = static_cast<USHORT>(base_valid / 2),
                    .Modified = static_cast<USHORT>(base_valid / 8),
                    .PageTables = static_cast<USHORT>(base_valid / 16),
                };

                c.emu.write_memory(system_information + header_size, &usage, sizeof(usage));
                c.emu.write_memory(system_information + header_size + sizeof(usage), usage_name.data(), sizeof(usage_name));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_logical_processor_and_group_information(const syscall_context& c, const uint64_t input_buffer,
                                                                    const uint32_t input_buffer_length, const uint64_t system_information,
                                                                    const uint32_t system_information_length,
                                                                    const emulator_object<uint32_t> return_length)
            {
                constexpr auto group_root_size = offsetof(EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64, Group);
                constexpr auto group_size = group_root_size + sizeof(EMU_GROUP_RELATIONSHIP64);
                constexpr auto numa_root_size = offsetof(EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64, NumaNode);
                constexpr auto numa_size = numa_root_size + sizeof(EMU_NUMA_NODE_RELATIONSHIP64);

                const auto write_group = [&](const uint64_t output_buffer) {
                    EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64 proc_info{};
                    proc_info.Size = group_size;
                    proc_info.Relationship = RelationGroup;

                    c.emu.write_memory(output_buffer, &proc_info, group_root_size);

                    EMU_GROUP_RELATIONSHIP64 group{};
                    group.ActiveGroupCount = 1;
                    group.MaximumGroupCount = 1;

                    auto& group_info = group.GroupInfo[0];
                    group_info.ActiveProcessorCount =
                        static_cast<uint8_t>(c.proc.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.ActiveProcessorCount; }));
                    group_info.ActiveProcessorMask = (1ULL << group_info.ActiveProcessorCount) - 1;
                    group_info.MaximumProcessorCount = group_info.ActiveProcessorCount;

                    c.emu.write_memory(output_buffer + group_root_size, group);
                };

                const auto write_numa = [&](const uint64_t output_buffer) {
                    EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64 proc_info{};
                    proc_info.Size = numa_size;
                    proc_info.Relationship = RelationNumaNode;

                    c.emu.write_memory(output_buffer, &proc_info, numa_root_size);

                    EMU_NUMA_NODE_RELATIONSHIP64 numa_node{};
                    memset(&numa_node, 0, sizeof(numa_node));

                    c.emu.write_memory(output_buffer + numa_root_size, numa_node);
                };

                if (input_buffer_length != sizeof(LOGICAL_PROCESSOR_RELATIONSHIP))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto request = c.emu.read_memory<LOGICAL_PROCESSOR_RELATIONSHIP>(input_buffer);

                if (request == RelationAll)
                {
                    constexpr auto required_size = group_size + numa_size;

                    if (return_length)
                    {
                        return_length.write(required_size);
                    }

                    if (system_information_length < required_size)
                    {
                        return STATUS_INFO_LENGTH_MISMATCH;
                    }

                    write_group(system_information);
                    write_numa(system_information + group_size);
                    return STATUS_SUCCESS;
                }

                if (request == RelationGroup)
                {
                    if (return_length)
                    {
                        return_length.write(group_size);
                    }

                    if (system_information_length < group_size)
                    {
                        return STATUS_INFO_LENGTH_MISMATCH;
                    }

                    write_group(system_information);
                    return STATUS_SUCCESS;
                }

                if (request == RelationNumaNode || request == RelationNumaNodeEx)
                {
                    if (return_length)
                    {
                        return_length.write(numa_size);
                    }

                    if (system_information_length < numa_size)
                    {
                        return STATUS_INFO_LENGTH_MISMATCH;
                    }

                    write_numa(system_information);
                    return STATUS_SUCCESS;
                }

                c.win_emu.log.error("Unsupported processor relationship: %X\n", request);
                c.emu.stop();
                return STATUS_NOT_SUPPORTED;
            }
        }

        constexpr uint64_t FAKE_KERNEL_BASE = 0xFFFFF80000000000ULL;
        constexpr uint32_t FAKE_KERNEL_SIZE = 0x00A00000;

        template <typename Traits>
        void fill_ntoskrnl_module(RTL_PROCESS_MODULE_INFORMATION<Traits>& m)
        {
            memset(&m, 0, sizeof(m));
            m.ImageBase = static_cast<typename Traits::PVOID>(FAKE_KERNEL_BASE);
            m.MappedBase = m.ImageBase;
            m.ImageSize = FAKE_KERNEL_SIZE;
            m.LoadCount = 1;

            constexpr std::string_view directory = R"(\SystemRoot\system32\)";
            constexpr std::string_view full_path = R"(\SystemRoot\system32\ntoskrnl.exe)";
            m.OffsetToFileName = static_cast<USHORT>(directory.size());
            memcpy(m.FullPathName, full_path.data(), full_path.size() + 1);
        }

        NTSTATUS handle_system_module_information(const syscall_context& c, const uint64_t system_information,
                                                  const uint32_t system_information_length, const emulator_object<uint32_t> return_length)
        {
            using Traits = EmulatorTraits<Emu64>;
            using modules_t = RTL_PROCESS_MODULES<Traits>;
            using module_t = RTL_PROCESS_MODULE_INFORMATION<Traits>;

            constexpr auto header_size = offsetof(modules_t, Modules);
            constexpr auto required = static_cast<uint32_t>(header_size + sizeof(module_t));

            if (return_length)
            {
                return_length.write(required);
            }

            if (system_information_length < required)
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            modules_t header{};
            memset(&header, 0, sizeof(header));
            header.NumberOfModules = 1;
            c.emu.write_memory(system_information, &header, header_size);

            module_t mod{};
            fill_ntoskrnl_module<Traits>(mod);
            c.emu.write_memory(system_information + header_size, &mod, sizeof(mod));

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_system_module_information_ex(const syscall_context& c, const uint64_t system_information,
                                                     const uint32_t system_information_length,
                                                     const emulator_object<uint32_t> return_length)
        {
            using Traits = EmulatorTraits<Emu64>;
            using module_ex_t = RTL_PROCESS_MODULE_INFORMATION_EX<Traits>;

            constexpr auto required = static_cast<uint32_t>(sizeof(module_ex_t));

            if (return_length)
            {
                return_length.write(required);
            }

            if (system_information_length < required)
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            module_ex_t entry{};
            memset(&entry, 0, sizeof(entry));
            entry.NextOffset = 0;
            fill_ntoskrnl_module<Traits>(entry.BaseInfo);
            entry.DefaultBase = entry.BaseInfo.ImageBase;

            c.emu.write_memory(system_information, &entry, sizeof(entry));
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_system_process_information(const syscall_context& c, const uint64_t system_information,
                                                   const uint32_t system_information_length, const emulator_object<uint32_t> return_length)
        {
            using Traits = EmulatorTraits<Emu64>;
            using proc_t = SYSTEM_PROCESS_INFORMATION<Traits>;
            using thread_t = SYSTEM_THREAD_INFORMATION<Traits>;

            uint64_t process_id = process_context::process_id;
            uint64_t active_tid = 0;
            if (c.vcpu.active_thread && c.vcpu.active_thread->teb64)
            {
                c.vcpu.active_thread->teb64->access([&](const TEB64& teb) {
                    process_id = teb.ClientId.UniqueProcess;
                    active_tid = teb.ClientId.UniqueThread;
                });
            }

            std::vector<uint64_t> thread_ids;
            for (const auto& t : c.proc.threads | std::views::values)
            {
                if (t.is_terminated() || !t.teb64)
                {
                    continue;
                }
                uint64_t tid = 0;
                t.teb64->access([&](const TEB64& teb) { tid = teb.ClientId.UniqueThread; });
                thread_ids.push_back(tid);
            }

            if (thread_ids.empty())
            {
                thread_ids.push_back(active_tid);
            }

            const auto required = static_cast<uint32_t>(sizeof(proc_t) + thread_ids.size() * sizeof(thread_t));

            if (return_length)
            {
                return_length.write(required);
            }

            if (system_information_length < required)
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            proc_t proc{};
            memset(&proc, 0, sizeof(proc));
            proc.NextEntryOffset = 0; // single process; terminator
            proc.NumberOfThreads = static_cast<ULONG>(thread_ids.size());
            proc.BasePriority = 8;
            proc.UniqueProcessId = process_id;
            proc.HandleCount = 0;
            proc.SessionId = 0;
            c.emu.write_memory(system_information, &proc, sizeof(proc));

            for (size_t i = 0; i < thread_ids.size(); ++i)
            {
                thread_t info{};
                memset(&info, 0, sizeof(info));
                info.ClientId.UniqueProcess = process_id;
                info.ClientId.UniqueThread = thread_ids[i];
                info.Priority = 8;
                info.BasePriority = 8;
                info.ThreadState = 5; // Waiting
                info.WaitReason = 6;  // WrQueue
                c.emu.write_memory(system_information + sizeof(proc_t) + i * sizeof(thread_t), &info, sizeof(info));
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, const uint32_t info_class, const uint64_t input_buffer,
                                                   const uint32_t input_buffer_length, const uint64_t system_information,
                                                   const uint32_t system_information_length, const emulator_object<uint32_t> return_length)
        {
            switch (info_class)
            {
            case SystemProcessInformation:
                return handle_system_process_information(c, system_information, system_information_length, return_length);

            case 250: // Build 27744
            case SystemFlushInformation:
            case SystemCodeIntegrityPolicyInformation:
            case SystemHypervisorSharedPageInformation:
            case SystemFeatureConfigurationInformation:
            case SystemSupportedProcessorArchitectures2:
            case SystemFeatureConfigurationSectionInformation:
            case SystemFirmwareTableInformation:
                return STATUS_NOT_SUPPORTED;

            case SystemControlFlowTransition:
                c.win_emu.callbacks.on_suspicious_activity("Warbird control flow transition");
                return STATUS_NOT_SUPPORTED;

            case SystemModuleInformation:
                return handle_system_module_information(c, system_information, system_information_length, return_length);

            case SystemModuleInformationEx:
                return handle_system_module_information_ex(c, system_information, system_information_length, return_length);

            case SystemTimeOfDayInformation:
                return handle_query<SYSTEM_TIMEOFDAY_INFORMATION64>(c.emu, system_information, system_information_length, return_length,
                                                                    [&](SYSTEM_TIMEOFDAY_INFORMATION64& info) {
                                                                        memset(&info, 0, sizeof(info));
                                                                        info.BootTime.QuadPart = 0;
                                                                        info.TimeZoneId = 0x00000002;
                                                                        // TODO: Fill
                                                                    });

            case SystemTimeZoneInformation:
            case SystemCurrentTimeZoneInformation:
                return handle_query<SYSTEM_TIMEZONE_INFORMATION>(
                    c.emu, system_information, system_information_length, return_length, [&](SYSTEM_TIMEZONE_INFORMATION& tzi) {
                        memset(&tzi, 0, sizeof(tzi));

                        tzi.Bias = -60;
                        tzi.StandardBias = 0;
                        tzi.DaylightBias = -60;

                        constexpr std::u16string_view std_name{u"W. Europe Standard Time"};
                        memcpy(&tzi.StandardName.arr[0], std_name.data(), std_name.size() * sizeof(char16_t));

                        constexpr std::u16string_view dlt_name{u"W. Europe Daylight Time"};
                        memcpy(&tzi.DaylightName.arr[0], dlt_name.data(), dlt_name.size() * sizeof(char16_t));

                        // Standard Time: Last Sunday in October, 03:00
                        tzi.StandardDate.wMonth = 10;
                        tzi.StandardDate.wDayOfWeek = 0;
                        tzi.StandardDate.wDay = 5;
                        tzi.StandardDate.wHour = 3;
                        tzi.StandardDate.wMinute = 0;
                        tzi.StandardDate.wSecond = 0;
                        tzi.StandardDate.wMilliseconds = 0;

                        // Daylight Time: Last Sunday in March, 02:00
                        tzi.DaylightDate.wMonth = 3;
                        tzi.DaylightDate.wDayOfWeek = 0;
                        tzi.DaylightDate.wDay = 5;
                        tzi.DaylightDate.wHour = 2;
                        tzi.DaylightDate.wMinute = 0;
                        tzi.DaylightDate.wSecond = 0;
                        tzi.DaylightDate.wMilliseconds = 0;
                    });

            case SystemDynamicTimeZoneInformation:
                return handle_query<SYSTEM_DYNAMIC_TIMEZONE_INFORMATION>(
                    c.emu, system_information, system_information_length, return_length, [&](SYSTEM_DYNAMIC_TIMEZONE_INFORMATION& dtzi) {
                        memset(&dtzi, 0, sizeof(dtzi));

                        dtzi.Bias = -60;
                        dtzi.StandardBias = 0;
                        dtzi.DaylightBias = -60;

                        constexpr std::u16string_view std_name{u"W. Europe Standard Time"};
                        memcpy(&dtzi.StandardName.arr[0], std_name.data(), std_name.size() * sizeof(char16_t));

                        constexpr std::u16string_view dlt_name{u"W. Europe Daylight Time"};
                        memcpy(&dtzi.DaylightName.arr[0], dlt_name.data(), dlt_name.size() * sizeof(char16_t));

                        constexpr std::u16string_view key_name{u"W. Europe Standard Time"};
                        memcpy(&dtzi.TimeZoneKeyName.arr[0], key_name.data(), key_name.size() * sizeof(char16_t));

                        // Standard Time: Last Sunday in October, 03:00
                        dtzi.StandardDate.wMonth = 10;
                        dtzi.StandardDate.wDayOfWeek = 0;
                        dtzi.StandardDate.wDay = 5;
                        dtzi.StandardDate.wHour = 3;
                        dtzi.StandardDate.wMinute = 0;
                        dtzi.StandardDate.wSecond = 0;
                        dtzi.StandardDate.wMilliseconds = 0;

                        // Daylight Time: Last Sunday in March, 02:00
                        dtzi.DaylightDate.wMonth = 3;
                        dtzi.DaylightDate.wDayOfWeek = 0;
                        dtzi.DaylightDate.wDay = 5;
                        dtzi.DaylightDate.wHour = 2;
                        dtzi.DaylightDate.wMinute = 0;
                        dtzi.DaylightDate.wSecond = 0;
                        dtzi.DaylightDate.wMilliseconds = 0;

                        dtzi.DynamicDaylightTimeDisabled = FALSE;
                    });

            case SystemMemoryUsageInformation:
            case SystemFullMemoryInformation:
            case SystemSummaryMemoryInformation:
                return handle_system_memory_usage_information(c, info_class, system_information, system_information_length, return_length);

            case SystemRangeStartInformation:
                return handle_query<SYSTEM_RANGE_START_INFORMATION64>(c.emu, system_information, system_information_length, return_length,
                                                                      [&](SYSTEM_RANGE_START_INFORMATION64& info) {
                                                                          info.SystemRangeStart = 0xFFFF800000000000; //
                                                                      });

            case SystemProcessorPerformanceInformation: {
                // Per-processor CPU time counters. We don't track real CPU time, so report an idle system:
                // IdleTime == KernelTime (Windows kernel time includes idle), UserTime == 0. Use the
                // executed-instruction tick as a monotonic clock so consecutive samples differ and callers
                // computing usage from deltas don't divide by zero.
                const auto processor_count = c.proc.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.ActiveProcessorCount; });
                constexpr auto entry_size = sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);
                const auto required = static_cast<uint32_t>(processor_count * entry_size);

                if (return_length)
                {
                    return_length.try_write(required);
                }

                if (system_information_length < required)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const auto tick = static_cast<int64_t>(c.win_emu.get_executed_instructions());
                std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> entries(processor_count);
                for (auto& entry : entries)
                {
                    memset(&entry, 0, sizeof(entry));
                    entry.IdleTime.QuadPart = tick;
                    entry.KernelTime.QuadPart = tick;
                }

                c.emu.write_memory(system_information, entries.data(), entries.size() * entry_size);
                return STATUS_SUCCESS;
            }

            case SystemProcessorInformation:
            case SystemEmulationProcessorInformation:
                return handle_query<SYSTEM_PROCESSOR_INFORMATION64>(
                    c.emu, system_information, system_information_length, return_length, [&](SYSTEM_PROCESSOR_INFORMATION64& info) {
                        memset(&info, 0, sizeof(info));
                        info.MaximumProcessors = 2;
                        info.ProcessorArchitecture =
                            (info_class == SystemProcessorInformation ? PROCESSOR_ARCHITECTURE_AMD64 : PROCESSOR_ARCHITECTURE_INTEL);
                    });

            case SystemNumaProcessorMap:
                return handle_query<SYSTEM_NUMA_INFORMATION64>(c.emu, system_information, system_information_length, return_length,
                                                               [&](SYSTEM_NUMA_INFORMATION64& info) {
                                                                   memset(&info, 0, sizeof(info));
                                                                   info.ActiveProcessorsGroupAffinity->Mask = 0xFFF;
                                                                   info.AvailableMemory[0] = 0xFFF;
                                                                   info.Pad[0] = 0xFFF;
                                                               });

            case SystemErrorPortTimeouts:
                return handle_query<SYSTEM_ERROR_PORT_TIMEOUTS>(c.emu, system_information, system_information_length, return_length,
                                                                [&](SYSTEM_ERROR_PORT_TIMEOUTS& info) {
                                                                    info.StartTimeout = 0;
                                                                    info.CommTimeout = 0;
                                                                });

            case SystemKernelDebuggerInformation:
                return handle_query<SYSTEM_KERNEL_DEBUGGER_INFORMATION>(c.emu, system_information, system_information_length, return_length,
                                                                        [&](SYSTEM_KERNEL_DEBUGGER_INFORMATION& info) {
                                                                            info.KernelDebuggerEnabled = FALSE;
                                                                            info.KernelDebuggerNotPresent = TRUE;
                                                                        });

            case SystemLogicalProcessorAndGroupInformation:
                return handle_logical_processor_and_group_information(c, input_buffer, input_buffer_length, system_information,
                                                                      system_information_length, return_length);

            case SystemLogicalProcessorInformation: {
                if (!input_buffer || input_buffer_length != sizeof(USHORT))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                using info_type = EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION<EmulatorTraits<Emu64>>;

                const auto processor_group = c.emu.read_memory<USHORT>(input_buffer);

                return handle_query<info_type>(c.emu, system_information, system_information_length, return_length, [&](info_type& info) {
                    info.Relationship = RelationProcessorCore;

                    if (processor_group == 0)
                    {
                        using mask_type = decltype(info.ProcessorMask);
                        const auto active_processor_count =
                            c.proc.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.ActiveProcessorCount; });
                        info.ProcessorMask = (static_cast<mask_type>(1) << active_processor_count) - 1;
                    }
                });
            }

            case SystemBasicInformation:
            case SystemEmulationBasicInformation:
                return handle_query<SYSTEM_BASIC_INFORMATION64>(
                    c.emu, system_information, system_information_length, return_length, [&](SYSTEM_BASIC_INFORMATION64& basic_info) {
                        basic_info.Reserved = 0;
                        basic_info.TimerResolution = 0x0002625a;
                        basic_info.PageSize = 0x1000;
                        basic_info.LowestPhysicalPageNumber = 0x00000001;
                        basic_info.HighestPhysicalPageNumber = 0x00c9c7ff;
                        basic_info.AllocationGranularity = ALLOCATION_GRANULARITY;
                        basic_info.MinimumUserModeAddress = MIN_ALLOCATION_ADDRESS;
                        basic_info.MaximumUserModeAddress = MAX_ALLOCATION_ADDRESS;
                        const auto processor_count =
                            c.proc.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.ActiveProcessorCount; });
                        basic_info.ActiveProcessorsAffinityMask = (processor_count >= 64) ? ~0ull : ((1ull << processor_count) - 1);
                        basic_info.NumberOfProcessors = static_cast<char>(processor_count);
                    });

            case SystemDeviceInformation:
                return handle_query<SYSTEM_DEVICE_INFORMATION>(c.emu, system_information, system_information_length, return_length,
                                                               [&](SYSTEM_DEVICE_INFORMATION& info) {
                                                                   memset(&info, 0, sizeof(info));
                                                                   info.NumberOfDisks = 1;
                                                               });

            case SystemExceptionInformation:
                return handle_query<SYSTEM_EXCEPTION_INFORMATION>(
                    c.emu, system_information, system_information_length, return_length,
                    [&](SYSTEM_EXCEPTION_INFORMATION& info) { memset(&info, 0, sizeof(info)); });

            case SystemLookasideInformation:
            case SystemPageFileInformation:
                // Variable-length lists we don't model (per-lookaside-list stats / configured page files).
                // Report an empty set.
                if (return_length)
                {
                    return_length.try_write(0);
                }
                return STATUS_SUCCESS;

            case SystemMemoryListInformation:
                return handle_query<SYSTEM_MEMORY_LIST_INFORMATION64>(
                    c.emu, system_information, system_information_length, return_length,
                    [&](SYSTEM_MEMORY_LIST_INFORMATION64& info) { memset(&info, 0, sizeof(info)); });

            case SystemFileCacheInformation:
            case SystemFileCacheInformationEx:
                return handle_query<SYSTEM_FILECACHE_INFORMATION64>(
                    c.emu, system_information, system_information_length, return_length,
                    [&](SYSTEM_FILECACHE_INFORMATION64& info) { memset(&info, 0, sizeof(info)); });

            case SystemPerformanceInformation:
            case SystemProcessorPowerInformation:
            case SystemInterruptInformation:
            case SystemProcessorIdleInformation:
                // SYSTEM_PERFORMANCE_INFORMATION is a large, OS-version-dependent counter block;
                // SystemProcessorPowerInformation / SystemInterruptInformation / SystemProcessorIdleInformation
                // are per-processor arrays. We don't model any of these, so report no activity by zeroing
                // exactly the caller's buffer, which keeps any struct revision/processor count happy instead
                // of rejecting an unexpected size.
                if (system_information)
                {
                    const std::vector<std::byte> zeros(system_information_length, std::byte{});
                    c.emu.write_memory(system_information, zeros.data(), zeros.size());
                }
                if (return_length)
                {
                    return_length.try_write(system_information_length);
                }
                return STATUS_SUCCESS;

            case SystemRecommendedSharedDataAlignment:
                return handle_query<ULONG>(c.emu, system_information, system_information_length, return_length,
                                           [&](ULONG& alignment) { alignment = 64; });

            case SystemNumaAvailableMemory:
                return handle_query<SYSTEM_NUMA_INFORMATION64>(c.emu, system_information, system_information_length, return_length,
                                                               [&](SYSTEM_NUMA_INFORMATION64& info) {
                                                                   memset(&info, 0, sizeof(info));
                                                                   info.AvailableMemory[0] = 0x80000000ull;
                                                               });

            case SystemSupportedProcessorArchitectures: {
                constexpr auto num_arch = 2;

                const auto required_length = sizeof(SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION) * (num_arch + 1);
                if (system_information_length < required_length)
                {
                    if (return_length)
                    {
                        return_length.try_write(required_length);
                    }

                    return STATUS_BUFFER_TOO_SMALL;
                }

                std::array<SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION, num_arch + 1> supported_arch{};
                supported_arch[0].Machine = IMAGE_FILE_MACHINE_AMD64;
                supported_arch[0].KernelMode = 1;
                supported_arch[0].UserMode = 1;
                supported_arch[0].Native = 1;
                supported_arch[1].Machine = IMAGE_FILE_MACHINE_I386;
                supported_arch[1].UserMode = 1;

                c.emu.write_memory(system_information, supported_arch);
                return STATUS_SUCCESS;
            }

            case SystemCodeIntegrityInformation: {
                // Report a normal retail configuration: code integrity (driver signature enforcement) enabled,
                // test-signing/debug off, so anti-tamper checks observe a non-tampered system.
                constexpr ULONG CODEINTEGRITY_OPTION_ENABLED = 0x1;
                return handle_query<SYSTEM_CODEINTEGRITY_INFORMATION>(c.emu, system_information, system_information_length, return_length,
                                                                      [&](SYSTEM_CODEINTEGRITY_INFORMATION& ci) {
                                                                          ci.Length = sizeof(ci);
                                                                          ci.CodeIntegrityOptions = CODEINTEGRITY_OPTION_ENABLED;
                                                                      });
            }

            default:
                c.win_emu.log.error("Unsupported system info class: 0x%X\n", info_class);
                c.emu.stop();
                return STATUS_NOT_SUPPORTED;
            }
        }

        NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, const uint32_t info_class, const uint64_t system_information,
                                                 const uint32_t system_information_length, const emulator_object<uint32_t> return_length)
        {
            return handle_NtQuerySystemInformationEx(c, info_class, 0, 0, system_information, system_information_length, return_length);
        }

        NTSTATUS handle_NtSetSystemInformation()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtPowerInformation(const syscall_context& c, const uint32_t information_level, const uint64_t /*input_buffer*/,
                                           const uint32_t /*input_buffer_length*/, const uint64_t output_buffer,
                                           const uint32_t output_buffer_length)
        {
            // POWER_INFORMATION_LEVEL: ProcessorInformation = 11 (per-CPU PROCESSOR_POWER_INFORMATION).
            // Games measure CPU speed by spinning until CurrentMhz/MhzLimit reports the CPU at full clock,
            // so CurrentMhz and MhzLimit are reported equal below (ratio 1.0) to let that loop converge.
            constexpr uint32_t processor_information = 11;

            struct processor_power_information
            {
                uint32_t number;
                uint32_t max_mhz;
                uint32_t current_mhz;
                uint32_t mhz_limit;
                uint32_t max_idle_state;
                uint32_t current_idle_state;
            };

            if (information_level == processor_information)
            {
                const uint32_t count =
                    output_buffer ? output_buffer_length / static_cast<uint32_t>(sizeof(processor_power_information)) : 0;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const processor_power_information info{
                        .number = i,
                        .max_mhz = 3000,
                        .current_mhz = 3000,
                        .mhz_limit = 3000,
                        .max_idle_state = 0,
                        .current_idle_state = 0,
                    };
                    c.emu.write_memory(output_buffer + static_cast<uint64_t>(i) * sizeof(info), &info, sizeof(info));
                }

                return STATUS_SUCCESS;
            }

            // Other levels (SystemPowerInformation, battery/power policy/state, ...): report a defined,
            // idle/AC-powered state by zeroing the caller's buffer. This satisfies the typical polled
            // "power/idle status" queries without modeling the full power subsystem.
            if (output_buffer && output_buffer_length > 0)
            {
                const std::vector<std::byte> zeros(output_buffer_length, std::byte{});
                c.emu.write_memory(output_buffer, zeros.data(), zeros.size());
            }

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
