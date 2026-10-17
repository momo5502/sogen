#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, handle process_handle, uint32_t process_information_class,
                                              uint64_t process_information, uint32_t process_information_length,
                                              emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    TEST(ProcessIdentityTest, DefaultUserProcessDoesNotUseReservedSystemIds)
    {
        const application_settings application{};
        const process_create_request request{};
        EXPECT_GT(application.process_id, 8u);
        EXPECT_EQ(application.process_id, request.process_id);
    }

    TEST(ProcessDeviceMapTest, ReportsMountedDrivesInNativeAndExtendedLayouts)
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
        ULONG expected_mask{};
        for (const auto drive : emu.file_sys.list_drives())
        {
            if (drive >= 'a' && drive <= 'z')
            {
                expected_mask |= 1u << (drive - 'a');
            }
        }
        ASSERT_NE(0, expected_mask);
        for (const auto size : {8u, 36u, 40u, 48u, 64u})
        {
            emu.memory.set_memory(storage + 0x100, 0, 64);
            emulator_object<ULONG>{emu.memory, storage}.write(0x12345678);
            const bool supported = size == 36 || size == 48;
            EXPECT_EQ(supported ? STATUS_SUCCESS : STATUS_INFO_LENGTH_MISMATCH,
                      syscalls::handle_NtQueryInformationProcess(context, CURRENT_PROCESS, ProcessDeviceMap, storage + 0x100, size,
                                                                 {emu.memory, storage}));
            EXPECT_EQ(supported ? size : 0x12345678, (emulator_object<ULONG>{emu.memory, storage}.read()));
            if (supported)
            {
                EXPECT_EQ(expected_mask, (emulator_object<ULONG>{emu.memory, storage + 0x100}.read()));
                for (size_t index = 0; index < 26; ++index)
                {
                    const auto type = emulator_object<uint8_t>{emu.memory, storage + 0x104 + index}.read();
                    EXPECT_EQ((expected_mask & (1u << index)) != 0 ? 3 : 0, type);
                }
            }
        }
    }
}
