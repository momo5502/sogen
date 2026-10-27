#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, handle process_handle, uint32_t info_class,
                                              uint64_t process_information, uint32_t process_information_length,
                                              emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        class ProcessGroupInformationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS query(const uint32_t size)
            {
                return syscalls::handle_NtQueryInformationProcess(this->context(), CURRENT_PROCESS, ProcessGroupInformation,
                                                                  this->storage + 0x100, size, {this->emu->memory, this->storage});
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(ProcessGroupInformationTest, ReportsTheRequiredSizeWhenTheBufferIsTooSmall)
    {
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(0));
        EXPECT_EQ(sizeof(USHORT), (emulator_object<uint32_t>{this->emu->memory, this->storage}.read()));
    }

    TEST_F(ProcessGroupInformationTest, ReportsProcessorGroupZero)
    {
        emulator_object<USHORT>{this->emu->memory, this->storage + 0x100}.write(7);
        ASSERT_EQ(STATUS_SUCCESS, this->query(sizeof(USHORT)));
        EXPECT_EQ(0, (emulator_object<USHORT>{this->emu->memory, this->storage + 0x100}.read()));
    }
}
