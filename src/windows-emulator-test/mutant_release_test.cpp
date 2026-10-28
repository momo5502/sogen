#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateMutant(const syscall_context& c, emulator_object<handle> mutant_handle, ACCESS_MASK desired_access,
                                   emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, BOOLEAN initial_owner);
    NTSTATUS handle_NtReleaseMutant(const syscall_context& c, handle mutant_handle, emulator_object<LONG> previous_count);
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtSignalAndWaitForSingleObject(const syscall_context& c, handle signal_handle, handle wait_handle, BOOLEAN alertable,
                                                   emulator_object<LARGE_INTEGER> timeout);
}

namespace sogen::test
{
    namespace
    {
        constexpr LONG untouched = static_cast<LONG>(0xAAAAAAAA);
        constexpr uint32_t other_thread_id = 0x44;

        class MutantReleaseTest : public testing::Test
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

            emulator_object<handle> handle_slot()
            {
                return {this->emu->memory, this->storage};
            }

            handle create_mutant(const bool owned)
            {
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateMutant(this->context(), this->handle_slot(), 0, {this->emu->memory},
                                                                          owned ? TRUE : FALSE));
                return this->handle_slot().read();
            }

            handle create_event()
            {
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateEvent(this->context(), this->handle_slot(), 0, {this->emu->memory},
                                                                         NotificationEvent, FALSE));
                return this->handle_slot().read();
            }

            mutant& object(const handle h)
            {
                return *this->emu->process.mutants.get(h);
            }

            NTSTATUS release(const handle mutant_handle, LONG& previous_count)
            {
                const emulator_object<LONG> previous{this->emu->memory, this->storage + 0x100};
                previous.write(untouched);
                const auto status = syscalls::handle_NtReleaseMutant(this->context(), mutant_handle, previous);
                previous_count = previous.read();
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(MutantReleaseTest, HandlesThatDoNotResolveAreInvalid)
    {
        const auto closed = this->create_mutant(true);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), closed));

        for (const auto h : {NULL_HANDLE, make_handle(0x1234), closed})
        {
            LONG previous{};
            EXPECT_EQ(STATUS_INVALID_HANDLE, this->release(h, previous)) << h.bits;
            EXPECT_EQ(untouched, previous) << h.bits;
        }
    }

    TEST_F(MutantReleaseTest, PseudoHandlesAreATypeMismatch)
    {
        for (const auto h : {CURRENT_PROCESS, CURRENT_THREAD})
        {
            LONG previous{};
            EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->release(h, previous)) << h.bits;
            EXPECT_EQ(untouched, previous) << h.bits;
        }
    }

    TEST_F(MutantReleaseTest, PreviousCountIsTheSignalStateBeforeTheRelease)
    {
        const auto h = this->create_mutant(true);
        const auto thread_id = this->context().thread().id;
        ASSERT_TRUE(this->object(h).try_lock(thread_id).has_value());
        ASSERT_TRUE(this->object(h).try_lock(thread_id).has_value());

        for (const LONG expected : {-2, -1, 0})
        {
            LONG previous{};
            EXPECT_EQ(STATUS_SUCCESS, this->release(h, previous)) << expected;
            EXPECT_EQ(expected, previous);
        }

        LONG previous{};
        EXPECT_EQ(STATUS_MUTANT_NOT_OWNED, this->release(h, previous));
        EXPECT_EQ(untouched, previous);
    }

    TEST_F(MutantReleaseTest, ReleasingAMutantTheCallerDoesNotOwnLeavesThePreviousCountUntouched)
    {
        const auto unowned = this->create_mutant(false);
        LONG previous{};
        EXPECT_EQ(STATUS_MUTANT_NOT_OWNED, this->release(unowned, previous));
        EXPECT_EQ(untouched, previous);

        const auto foreign = this->create_mutant(false);
        ASSERT_TRUE(this->object(foreign).try_lock(other_thread_id).has_value());
        EXPECT_EQ(STATUS_MUTANT_NOT_OWNED, this->release(foreign, previous));
        EXPECT_EQ(untouched, previous);
    }

    TEST_F(MutantReleaseTest, ReleasingAnAbandonedMutantReportsAbandoned)
    {
        const auto h = this->create_mutant(false);
        ASSERT_TRUE(this->object(h).try_lock(other_thread_id).has_value());
        this->object(h).abandon();

        LONG previous{};
        EXPECT_EQ(STATUS_ABANDONED, this->release(h, previous));
        EXPECT_EQ(untouched, previous);
        EXPECT_TRUE(this->object(h).abandoned);
    }

    TEST_F(MutantReleaseTest, SignalAndWaitOnAnAbandonedMutantReturnsWithoutWaiting)
    {
        const auto h = this->create_mutant(false);
        ASSERT_TRUE(this->object(h).try_lock(other_thread_id).has_value());
        this->object(h).abandon();

        const emulator_object<LARGE_INTEGER> timeout{this->emu->memory, this->storage + 0x200};
        timeout.write(LARGE_INTEGER{});

        EXPECT_EQ(STATUS_ABANDONED,
                  syscalls::handle_NtSignalAndWaitForSingleObject(this->context(), h, this->create_event(), FALSE, timeout));
        EXPECT_TRUE(this->object(h).abandoned);
    }
}
