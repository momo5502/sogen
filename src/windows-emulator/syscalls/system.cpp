#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    namespace
    {
        NTSTATUS handle_logical_processor_and_group_information(const syscall_context& c, const uint64_t input_buffer,
                                                                const uint32_t input_buffer_length, const uint64_t system_information,
                                                                const uint32_t system_information_length,
                                                                const emulator_object<uint32_t> return_length)
        {
            if (input_buffer_length != sizeof(LOGICAL_PROCESSOR_RELATIONSHIP))
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto request = c.emu.read_memory<LOGICAL_PROCESSOR_RELATIONSHIP>(input_buffer);

            if (request == RelationGroup)
            {
                constexpr auto root_size = offsetof(EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64, Group);
                constexpr auto required_size = root_size + sizeof(EMU_GROUP_RELATIONSHIP64);

                if (return_length)
                {
                    return_length.write(required_size);
                }

                if (system_information_length < required_size)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64 proc_info{};
                proc_info.Size = required_size;
                proc_info.Relationship = RelationGroup;

                c.emu.write_memory(system_information, &proc_info, root_size);

                EMU_GROUP_RELATIONSHIP64 group{};
                group.ActiveGroupCount = 1;
                group.MaximumGroupCount = 1;

                auto& group_info = group.GroupInfo[0];
                group_info.ActiveProcessorCount = static_cast<uint8_t>(c.proc.kusd.get().ActiveProcessorCount);
                group_info.ActiveProcessorMask = (1 << group_info.ActiveProcessorCount) - 1;
                group_info.MaximumProcessorCount = group_info.ActiveProcessorCount;

                c.emu.write_memory(system_information + root_size, group);
                return STATUS_SUCCESS;
            }

            if (request == RelationNumaNode || request == RelationNumaNodeEx)
            {
                constexpr auto root_size = offsetof(EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64, NumaNode);
                constexpr auto required_size = root_size + sizeof(EMU_NUMA_NODE_RELATIONSHIP64);

                if (return_length)
                {
                    return_length.write(required_size);
                }

                if (system_information_length < required_size)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64 proc_info{};
                proc_info.Size = required_size;
                proc_info.Relationship = RelationNumaNode;

                c.emu.write_memory(system_information, &proc_info, root_size);

                EMU_NUMA_NODE_RELATIONSHIP64 numa_node{};
                memset(&numa_node, 0, sizeof(numa_node));

                c.emu.write_memory(system_information + root_size, numa_node);
                return STATUS_SUCCESS;
            }

            c.win_emu.log.error("Unsupported processor relationship: %X\n", request);
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }
    }

    NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, const uint32_t info_class, const uint64_t input_buffer,
                                               const uint32_t input_buffer_length, const uint64_t system_information,
                                               const uint32_t system_information_length, const emulator_object<uint32_t> return_length)
    {
        switch (info_class)
        {
        case 250: // Build 27744
        case SystemFlushInformation:
        case SystemModuleInformation:
        case SystemProcessInformation:
        case SystemMemoryUsageInformation:
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

        case SystemRangeStartInformation:
            return handle_query<SYSTEM_RANGE_START_INFORMATION64>(c.emu, system_information, system_information_length, return_length,
                                                                  [&](SYSTEM_RANGE_START_INFORMATION64& info) {
                                                                      info.SystemRangeStart = 0xFFFF800000000000; //
                                                                  });

        case SystemProcessorInformation:
            return handle_query<SYSTEM_PROCESSOR_INFORMATION64>(c.emu, system_information, system_information_length, return_length,
                                                                [&](SYSTEM_PROCESSOR_INFORMATION64& info) {
                                                                    memset(&info, 0, sizeof(info));
                                                                    info.MaximumProcessors = 2;
                                                                    info.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
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
                    const auto active_processor_count = c.proc.kusd.get().ActiveProcessorCount;
                    info.ProcessorMask = (static_cast<mask_type>(1) << active_processor_count) - 1;
                }
            });
        }

        case SystemBasicInformation:
        case SystemEmulationBasicInformation:
            return handle_query<SYSTEM_BASIC_INFORMATION64>(c.emu, system_information, system_information_length, return_length,
                                                            [&](SYSTEM_BASIC_INFORMATION64& basic_info) {
                                                                basic_info.Reserved = 0;
                                                                basic_info.TimerResolution = 0x0002625a;
                                                                basic_info.PageSize = 0x1000;
                                                                basic_info.LowestPhysicalPageNumber = 0x00000001;
                                                                basic_info.HighestPhysicalPageNumber = 0x00c9c7ff;
                                                                basic_info.AllocationGranularity = ALLOCATION_GRANULARITY;
                                                                basic_info.MinimumUserModeAddress = MIN_ALLOCATION_ADDRESS;
                                                                basic_info.MaximumUserModeAddress = MAX_ALLOCATION_ADDRESS;
                                                                basic_info.ActiveProcessorsAffinityMask = 0x0000000000000f;
                                                                basic_info.NumberOfProcessors = 4;
                                                            });

        default:
            c.win_emu.log.error("Unsupported system info class: %X\n", info_class);
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
}
