#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, handle thread_handle, THREADINFOCLASS info_class,
                                           uint64_t thread_information, uint32_t thread_information_length);
    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, handle thread_handle, uint32_t info_class,
                                             uint64_t thread_information, uint32_t thread_information_length,
                                             emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t untouched = 0xAAAAAAAA;

        class ThreadInformationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true}));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS set(const uint32_t info_class, const uint64_t buffer, const uint32_t length)
            {
                return syscalls::handle_NtSetInformationThread(this->context(), CURRENT_THREAD, static_cast<THREADINFOCLASS>(info_class),
                                                               buffer, length);
            }

            NTSTATUS query(const uint32_t info_class, const uint64_t buffer, const uint32_t length, uint32_t& return_length)
            {
                const emulator_object<uint32_t> returned{this->emu->memory, this->storage + 0x800};
                returned.write(untouched);
                const auto status =
                    syscalls::handle_NtQueryInformationThread(this->context(), CURRENT_THREAD, info_class, buffer, length, returned);
                return_length = returned.read();
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(ThreadInformationTest, SettingAnUnknownInformationClassIsRefused)
    {
        for (const uint32_t info_class : {0x7FFFu, 0x100u, 0xFFFFFFFFu})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->set(info_class, this->storage, sizeof(ULONG))) << info_class;
        }
    }

    TEST_F(ThreadInformationTest, QueryingAnUnknownInformationClassIsRefusedWithoutAReturnLength)
    {
        for (const uint32_t info_class : {0x7FFFu, 0x100u})
        {
            uint32_t return_length{};
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query(info_class, this->storage, 64, return_length)) << info_class;
            EXPECT_EQ(untouched, return_length) << info_class;
        }
    }

    TEST_F(ThreadInformationTest, ThreadNameInformationReturnsTheNameBehindTheStructure)
    {
        this->emu->vcpu(0).active_thread->name = u"probe";
        constexpr uint32_t total_size = sizeof(THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>) + 5 * sizeof(char16_t);

        uint32_t return_length{};
        for (const uint32_t length : {0u, 8u, 16u, total_size - 1})
        {
            EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(ThreadNameInformation, this->storage, length, return_length)) << length;
            EXPECT_EQ(total_size, return_length) << length;
        }

        ASSERT_EQ(STATUS_SUCCESS, this->query(ThreadNameInformation, this->storage, 0x400, return_length));
        EXPECT_EQ(total_size, return_length);

        const emulator_object<THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>> information{this->emu->memory, this->storage};
        const auto name = information.read().ThreadName;
        EXPECT_EQ(5 * sizeof(char16_t), name.Length);
        EXPECT_EQ(this->storage + sizeof(THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>), name.Buffer);

        std::u16string contents(5, u'\0');
        this->emu->memory.read_memory(name.Buffer, contents.data(), name.Length);
        EXPECT_EQ(u"probe", contents);
    }

    TEST_F(ThreadInformationTest, ThreadNameInformationOfAnUnnamedThreadIsEmpty)
    {
        uint32_t return_length{};
        ASSERT_EQ(STATUS_SUCCESS, this->query(ThreadNameInformation, this->storage, 0x400, return_length));
        EXPECT_EQ(sizeof(THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>), return_length);
        const emulator_object<THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>> information{this->emu->memory, this->storage};
        EXPECT_EQ(0u, information.read().ThreadName.Length);
    }

    TEST_F(ThreadInformationTest, HideFromDebuggerIgnoresTheBufferWhenTheLengthIsZero)
    {
        const auto misaligned = this->storage + 1;
        EXPECT_EQ(STATUS_SUCCESS, this->set(ThreadHideFromDebugger, misaligned, 0));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->set(ThreadHideFromDebugger, misaligned, 1));
    }

    TEST_F(ThreadInformationTest, ImpersonationTokenRequiresTheLengthOfAHandle)
    {
        for (const uint32_t length : {0u, 4u, 16u})
        {
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->set(ThreadImpersonationToken, this->storage, length)) << length;
        }

        emulator_object<handle>{this->emu->memory, this->storage}.write({});
        EXPECT_EQ(STATUS_SUCCESS, this->set(ThreadImpersonationToken, this->storage, sizeof(handle)));
    }
}
