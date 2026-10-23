#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, uint32_t info_class, uint64_t input_buffer,
                                               uint32_t input_buffer_length, uint64_t system_information,
                                               uint32_t system_information_length, emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    // The thread pool maps a thread's ideal processor to a NUMA node; a node without processors makes it
    // index past its per-node queues.
    TEST(ProcessorTopologyTest, NumaNodeContainsActiveProcessors)
    {
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::make_unique<null_console_backend>();
        windows_emulator emu(create_x86_64_emulator_from_environment(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             emulator_callbacks{}, std::move(interfaces));
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        constexpr uint32_t processor_count = 4;
        emu.process.kusd.access([](KUSER_SHARED_DATA64& kusd) { kusd.ActiveProcessorCount = processor_count; });

        emu.memory.write_memory(storage, LOGICAL_PROCESSOR_RELATIONSHIP{RelationNumaNode});
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQuerySystemInformationEx(context, SystemLogicalProcessorAndGroupInformation, storage,
                                                                              sizeof(LOGICAL_PROCESSOR_RELATIONSHIP), storage + 0x100,
                                                                              0x200, {emu.memory, storage + 0x10}));
        const auto information = emu.memory.read_memory<EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64>(storage + 0x100);
        EXPECT_EQ(RelationNumaNode, information.Relationship);
        EXPECT_EQ(0u, information.NumaNode.NodeNumber);
        EXPECT_EQ(1u, information.NumaNode.GroupCount);
        EXPECT_EQ(0u, information.NumaNode.GroupMask.Group);
        EXPECT_EQ(0xFu, information.NumaNode.GroupMask.Mask);
    }

    TEST(ProcessorTopologyTest, SupportedArchitecturesMarkTheQueriedProcess)
    {
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::make_unique<null_console_backend>();
        windows_emulator emu(create_x86_64_emulator_from_environment(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             emulator_callbacks{}, std::move(interfaces));
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        const auto query = [&](const handle process, const uint32_t input_length = sizeof(handle)) {
            emu.memory.write_memory(storage, process);
            emu.memory.set_memory(storage + 0x100, 0, 0x20);
            return syscalls::handle_NtQuerySystemInformationEx(context, SystemSupportedProcessorArchitectures, storage, input_length,
                                                               storage + 0x100, 0x20, {emu.memory, storage + 0x10});
        };
        const auto entries = [&] { return emu.memory.read_memory<std::array<uint32_t, 3>>(storage + 0x100); };

        // Native x64 Windows: AMD64 kernel/user/native (+ process), x86 user + WoW64 container, terminator.
        ASSERT_EQ(STATUS_SUCCESS, query(CURRENT_PROCESS));
        EXPECT_EQ(entries(), (std::array<uint32_t, 3>{0x000F8664, 0x0012014C, 0}));
        ASSERT_EQ(STATUS_SUCCESS, query(handle{}));
        EXPECT_EQ(entries(), (std::array<uint32_t, 3>{0x00078664, 0x0012014C, 0}));
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, query(CURRENT_THREAD));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, query(CURRENT_PROCESS, 0));

        emu.process.is_wow64_process = true;
        ASSERT_EQ(STATUS_SUCCESS, query(CURRENT_PROCESS));
        EXPECT_EQ(entries(), (std::array<uint32_t, 3>{0x00078664, 0x001A014C, 0}));
    }

    TEST(ProcessorTopologyTest, SupportedArchitecturesRejectUnreadableProcessHandles)
    {
        windows_emulator emu{create_arm64_emulator(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false}};
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        constexpr std::array<uint32_t, 3> sentinel{0x11111111, 0x22222222, 0x33333333};
        constexpr uint32_t length_sentinel = 0xDEADBEEF;
        const auto output = storage + 0x100;
        const auto return_length = storage + 0x10;
        const std::array inputs{uint64_t{0}, uint64_t{0xDEAD0000}, storage + 0x1000 - sizeof(uint32_t)};

        for (const auto input : inputs)
        {
            emu.memory.write_memory(output, sentinel);
            emu.memory.write_memory(return_length, length_sentinel);
            EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                      syscalls::handle_NtQuerySystemInformationEx(context, SystemSupportedProcessorArchitectures, input, sizeof(handle),
                                                                  output, sizeof(sentinel), {emu.memory, return_length}));
            EXPECT_EQ((emu.memory.read_memory<std::array<uint32_t, 3>>(output)), sentinel);
            EXPECT_EQ(emu.memory.read_memory<uint32_t>(return_length), length_sentinel);
        }
    }
}
