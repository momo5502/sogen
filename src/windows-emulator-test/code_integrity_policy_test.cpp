#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                             uint32_t system_information_length, emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    TEST(CodeIntegrityPolicyTest, ReportsNoActiveGuestPolicyWithNativeBufferContract)
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
        for (const auto size : {0u, 16u, 31u, 32u, 40u})
        {
            emu.memory.set_memory(storage + 0x100, 0xab, 40);
            EXPECT_EQ(size == 32 ? STATUS_SUCCESS : STATUS_INFO_LENGTH_MISMATCH,
                      syscalls::handle_NtQuerySystemInformation(context, SystemCodeIntegrityPolicyInformation, storage + 0x100, size,
                                                                {emu.memory, storage}));
            EXPECT_EQ(32u, (emulator_object<uint32_t>{emu.memory, storage}.read()));
            const auto result = emulator_object<std::array<uint8_t, 40>>{emu.memory, storage + 0x100}.read();
            for (size_t index = 0; index < result.size(); ++index)
            {
                EXPECT_EQ(size == 32 && index < 32 ? 0 : 0xab, result[index]);
            }
        }
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtQuerySystemInformation(context, SystemCodeIntegrityPolicyInformation,
                                                                                     0x12345678, 32, {emu.memory, storage}));
        EXPECT_EQ(32u, emu.memory.read_memory<uint32_t>(storage));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtQuerySystemInformation(context, SystemCodeIntegrityPolicyInformation,
                                                                                     storage + 0x100, 32, {emu.memory, 0x12345678}));
        EXPECT_EQ(0xabu, emu.memory.read_memory<uint8_t>(storage + 0x100));
    }
}
