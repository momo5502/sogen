#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationJobObject(const syscall_context& c, handle job_handle, uint32_t info_class, uint64_t info,
                                              ULONG length);
    NTSTATUS handle_NtAssignProcessToJobObject(const syscall_context& c, handle job_handle, handle process_handle);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t extended_limit_information_class = 9;
        constexpr ULONG extended_limit_information_size = 144;
        constexpr uint64_t limit_flags_offset = 16;

        class JobObjectTest : public testing::Test
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
                this->job = this->emu->process.jobs.store(job_object{});
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS set_limit_flags(const uint32_t flags)
            {
                this->emu->emu().write_memory(this->storage + limit_flags_offset, &flags, sizeof(flags));
                return syscalls::handle_NtSetInformationJobObject(this->context(), this->job, extended_limit_information_class,
                                                                  this->storage, extended_limit_information_size);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            handle job{};
        };
    }

    TEST_F(JobObjectTest, AcceptsTheLimitFlagsLibuvSets)
    {
        constexpr uint32_t die_on_unhandled_exception = 0x0400;
        constexpr uint32_t breakaway_ok = 0x0800;
        constexpr uint32_t silent_breakaway_ok = 0x1000;
        constexpr uint32_t kill_on_job_close = 0x2000;
        constexpr auto flags = die_on_unhandled_exception | breakaway_ok | silent_breakaway_ok | kill_on_job_close;

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit_flags(flags));
        EXPECT_EQ(flags, this->emu->process.jobs.get(this->job)->limit_flags);
    }

    TEST_F(JobObjectTest, RejectsLimitsThatAreNotEnforced)
    {
        constexpr uint32_t active_process_limit = 0x0008;
        EXPECT_EQ(STATUS_NOT_SUPPORTED, this->set_limit_flags(active_process_limit));
    }

    TEST_F(JobObjectTest, AssignsTheCurrentProcess)
    {
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtAssignProcessToJobObject(this->context(), this->job, CURRENT_PROCESS));
    }

    TEST_F(JobObjectTest, RejectsInvalidHandlesWhenAssigning)
    {
        EXPECT_EQ(STATUS_INVALID_HANDLE,
                  syscalls::handle_NtAssignProcessToJobObject(this->context(), handle{.bits = 0x1234}, CURRENT_PROCESS));
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtAssignProcessToJobObject(this->context(), this->job, handle{.bits = 0x1234}));
    }
}
