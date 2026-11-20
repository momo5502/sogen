#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtSetEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
    NTSTATUS handle_NtPulseEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
    NTSTATUS handle_NtClearEvent(const syscall_context& c, handle event_handle);
}

namespace sogen::test
{
    namespace
    {
        // KeSetEvent satisfies the waits it releases before it returns, so a reset that follows cannot take a registered
        // waiter's wake back.
        class EventWaitCommitTest : public testing::TestWithParam<EVENT_TYPE>
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu =
                    std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                       emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root()},
                                                       emulator_callbacks{}, std::move(interfaces));
                this->signaler = this->add_thread(8);
                this->waiter = this->add_thread(12);
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->signaler);

                const auto storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->allocator.emplace(this->emu->memory, storage, 0x1000);

                const auto event_handle = this->allocator->reserve<handle>();
                ASSERT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtCreateEvent(this->context(), event_handle, 0, {this->emu->memory}, GetParam(), FALSE));
                this->event = event_handle.read();
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            handle add_thread(const uint32_t id)
            {
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true});
            }

            emulator_thread& waiting_thread()
            {
                return *this->emu->process.threads.get(this->waiter);
            }

            void register_wait()
            {
                auto& thread = this->waiting_thread();
                thread.await_objects = {*this->emu->process.events.reference_of(this->event)};
                thread.await_any = true;
                ASSERT_FALSE(thread.is_thread_ready(*this->emu));
            }

            std::unique_ptr<windows_emulator> emu{};
            std::optional<emulator_allocator> allocator{};
            handle signaler{};
            handle waiter{};
            handle event{};
        };

        TEST_P(EventWaitCommitTest, ResetAfterSetKeepsTheWake)
        {
            this->register_wait();

            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetEvent(this->context(), this->event.bits, {this->emu->memory}));
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClearEvent(this->context(), this->event));

            EXPECT_TRUE(this->waiting_thread().is_thread_ready(*this->emu));
            EXPECT_EQ(this->waiting_thread().pending_status, std::optional<NTSTATUS>{STATUS_WAIT_0});
        }

        TEST_P(EventWaitCommitTest, PulseWakesTheRegisteredWaiter)
        {
            this->register_wait();

            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtPulseEvent(this->context(), this->event.bits, {this->emu->memory}));

            EXPECT_TRUE(this->waiting_thread().is_thread_ready(*this->emu));
            EXPECT_EQ(this->waiting_thread().pending_status, std::optional<NTSTATUS>{STATUS_WAIT_0});
        }

        INSTANTIATE_TEST_SUITE_P(EventTypes, EventWaitCommitTest, testing::Values(SynchronizationEvent, NotificationEvent));
    }
}
