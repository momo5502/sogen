#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, const uint32_t info_class,
                                               const uint64_t input_buffer, const uint32_t input_buffer_length,
                                               const uint64_t system_information,
                                               const uint32_t system_information_length,
                                               const emulator_object<uint32_t> return_length)
    {
        if (info_class == SystemFlushInformation                    //
            || info_class == SystemHypervisorSharedPageInformation  //
            || info_class == 250                                    // Build 27744
            || info_class == SystemProcessInformation               //
            || info_class == SystemModuleInformation                //
            || info_class == SystemMemoryUsageInformation           //
            || info_class == SystemCodeIntegrityPolicyInformation   //
            || info_class == SystemFeatureConfigurationInformation  //
            || info_class == SystemSupportedProcessorArchitectures2 //
            || info_class == SystemFeatureConfigurationSectionInformation)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == SystemTimeOfDayInformation)
        {
            return handle_query<SYSTEM_TIMEOFDAY_INFORMATION64>(c.emu, system_information, system_information_length,
                                                                return_length,
                                                                [&](SYSTEM_TIMEOFDAY_INFORMATION64& info) {
                                                                    info.BootTime.QuadPart = 0;
                                                                    // TODO: Fill
                                                                });
        }

        if (info_class == SystemRangeStartInformation)
        {
            return handle_query<SYSTEM_RANGE_START_INFORMATION64>(c.emu, system_information, system_information_length,
                                                                  return_length,
                                                                  [&](SYSTEM_RANGE_START_INFORMATION64& info) {
                                                                      info.SystemRangeStart = 0xFFFF800000000000; //
                                                                  });
        }

        if (info_class == SystemProcessorInformation)
        {
            return handle_query<SYSTEM_PROCESSOR_INFORMATION64>(
                c.emu, system_information, system_information_length, return_length,
                [&](SYSTEM_PROCESSOR_INFORMATION64& info) {
                    memset(&info, 0, sizeof(info));
                    info.MaximumProcessors = 2;
                    info.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
                });
        }

        if (info_class == SystemNumaProcessorMap)
        {
            return handle_query<SYSTEM_NUMA_INFORMATION64>(c.emu, system_information, system_information_length,
                                                           return_length, [&](SYSTEM_NUMA_INFORMATION64& info) {
                                                               memset(&info, 0, sizeof(info));
                                                               info.ActiveProcessorsGroupAffinity->Mask = 0xFFF;
                                                               info.AvailableMemory[0] = 0xFFF;
                                                               info.Pad[0] = 0xFFF;
                                                           });
        }

        if (info_class == SystemErrorPortTimeouts)
        {
            return handle_query<SYSTEM_ERROR_PORT_TIMEOUTS>(c.emu, system_information, system_information_length,
                                                            return_length, [&](SYSTEM_ERROR_PORT_TIMEOUTS& info) {
                                                                info.StartTimeout = 0;
                                                                info.CommTimeout = 0;
                                                            });
        }

        if (info_class == SystemKernelDebuggerInformation)
        {
            return handle_query<SYSTEM_KERNEL_DEBUGGER_INFORMATION>(c.emu, system_information,
                                                                    system_information_length, return_length,
                                                                    [&](SYSTEM_KERNEL_DEBUGGER_INFORMATION& info) {
                                                                        info.KernelDebuggerEnabled = FALSE;
                                                                        info.KernelDebuggerNotPresent = TRUE;
                                                                    });
        }

        if (info_class == SystemLogicalProcessorInformation)
        {
            if (input_buffer_length != sizeof(USHORT))
            {
                return STATUS_INVALID_PARAMETER;
            }

            using INFO_TYPE = EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION<EmulatorTraits<Emu64>>;

            const auto processor_group = c.emu.read_memory<USHORT>(input_buffer);
            constexpr auto required_size = sizeof(INFO_TYPE);

            if (return_length)
            {
                return_length.write(required_size);
            }

            if (system_information_length < required_size)
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            INFO_TYPE information{};
            information.Relationship = RelationProcessorCore;

            if (processor_group == 0)
            {
                const auto active_processor_count = c.proc.kusd.get().ActiveProcessorCount;
                information.ProcessorMask =
                    (static_cast<decltype(information.ProcessorMask)>(1) << active_processor_count) - 1;
            }

            c.emu.write_memory(system_information, information);
            return STATUS_SUCCESS;
        }

        if (info_class == SystemLogicalProcessorAndGroupInformation)
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

        if (info_class == SystemControlFlowTransition)
        {
            c.win_emu.log.print(color::pink, "Warbird control flow transition!\n");
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == SystemBasicInformation || info_class == SystemEmulationBasicInformation)
        {
            return handle_query<SYSTEM_BASIC_INFORMATION64>(
                c.emu, system_information, system_information_length, return_length,
                [&](SYSTEM_BASIC_INFORMATION64& basic_info) {
                    basic_info.Reserved = 0;
                    basic_info.TimerResolution = 0x0002625a;
                    basic_info.PageSize = 0x1000;
                    basic_info.LowestPhysicalPageNumber = 0x00000001;
                    basic_info.HighestPhysicalPageNumber = 0x00c9c7ff;
                    basic_info.AllocationGranularity = ALLOCATION_GRANULARITY;
                    basic_info.MinimumUserModeAddress = MIN_ALLOCATION_ADDRESS;
                    basic_info.MaximumUserModeAddress = MAX_ALLOCATION_ADDRESS;
                    basic_info.ActiveProcessorsAffinityMask = 0x0000000000000fff;
                    basic_info.NumberOfProcessors = 1;
                });
        }

        c.win_emu.log.error("Unsupported system info class: %X\n", info_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, const uint32_t info_class,
                                             const uint64_t system_information,
                                             const uint32_t system_information_length,
                                             const emulator_object<uint32_t> return_length)
    {
        return handle_NtQuerySystemInformationEx(c, info_class, 0, 0, system_information, system_information_length,
                                                 return_length);
    }

    NTSTATUS handle_NtSetSystemInformation()
    {
        return STATUS_NOT_SUPPORTED;
    }
}
