#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateMutant(const syscall_context& c, emulator_object<handle> mutant_handle, ACCESS_MASK desired_access,
                                   emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, BOOLEAN initial_owner);
    NTSTATUS handle_NtQueryMutant(const syscall_context& c, handle mutant_handle, ULONG mutant_information_class,
                                  uint64_t mutant_information, ULONG mutant_information_length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, LONG initial_count,
                                      LONG maximum_count);
    NTSTATUS handle_NtQuerySemaphore(const syscall_context& c, handle semaphore_handle, ULONG semaphore_information_class,
                                     emulator_object<SEMAPHORE_BASIC_INFORMATION> semaphore_information, ULONG semaphore_information_length,
                                     emulator_object<ULONG> return_length);
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtResetEvent(const syscall_context& c, handle event_handle, emulator_object<LONG> previous_state);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint8_t untouched_byte = 0xAA;
        constexpr ULONG untouched_length = 0xAAAAAAAA;
        constexpr LONG untouched_state = static_cast<LONG>(0xAAAAAAAA);
        constexpr uint32_t other_thread_id = 0x44;
        constexpr size_t buffer_size = 32;

        using buffer_bytes = std::array<uint8_t, buffer_size>;

        buffer_bytes untouched_buffer()
        {
            buffer_bytes bytes{};
            bytes.fill(untouched_byte);
            return bytes;
        }

        struct query_result
        {
            NTSTATUS status{};
            ULONG return_length{};
            buffer_bytes bytes{};
        };

        class SyncQueryTest : public testing::Test
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

            uint64_t buffer_address() const
            {
                return this->storage + 0x100;
            }

            emulator_object<ULONG> return_length_slot()
            {
                return {this->emu->memory, this->storage + 0x200};
            }

            void reset_outputs()
            {
                const auto bytes = untouched_buffer();
                this->emu->memory.write_memory(this->buffer_address(), bytes.data(), bytes.size());
                this->return_length_slot().write(untouched_length);
            }

            query_result read_outputs(const NTSTATUS status)
            {
                query_result result{};
                result.status = status;
                result.return_length = this->return_length_slot().read();
                this->emu->memory.read_memory(this->buffer_address(), result.bytes.data(), result.bytes.size());
                return result;
            }

            handle create_mutant(const bool owned)
            {
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateMutant(this->context(), this->handle_slot(), 0, {this->emu->memory},
                                                                          owned ? TRUE : FALSE));
                return this->handle_slot().read();
            }

            handle create_semaphore(const LONG initial_count, const LONG maximum_count)
            {
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateSemaphore(this->context(), this->handle_slot(), 0, {this->emu->memory},
                                                                             initial_count, maximum_count));
                return this->handle_slot().read();
            }

            handle create_event(const EVENT_TYPE type, const bool signaled)
            {
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateEvent(this->context(), this->handle_slot(), 0, {this->emu->memory}, type,
                                                                         signaled ? TRUE : FALSE));
                return this->handle_slot().read();
            }

            handle closed_handle(const handle h)
            {
                const auto status = syscalls::handle_NtClose(this->context(), h);
                EXPECT_EQ(STATUS_SUCCESS, status);
                return h;
            }

            mutant& mutant_object(const handle h)
            {
                return *this->emu->process.mutants.get(h);
            }

            query_result query_mutant(const handle h, const ULONG information_class, const ULONG length)
            {
                this->reset_outputs();
                return this->read_outputs(syscalls::handle_NtQueryMutant(this->context(), h, information_class, this->buffer_address(),
                                                                         length, this->return_length_slot()));
            }

            MUTANT_BASIC_INFORMATION query_mutant_basic(const handle h)
            {
                const auto result = this->query_mutant(h, MutantBasicInformation, sizeof(MUTANT_BASIC_INFORMATION));
                EXPECT_EQ(STATUS_SUCCESS, result.status);
                EXPECT_EQ(sizeof(MUTANT_BASIC_INFORMATION), result.return_length);
                EXPECT_EQ(untouched_byte, result.bytes[6]);
                EXPECT_EQ(untouched_byte, result.bytes[7]);
                return emulator_object<MUTANT_BASIC_INFORMATION>{this->emu->memory, this->buffer_address()}.read();
            }

            CLIENT_ID64 query_mutant_owner(const handle h)
            {
                const auto result = this->query_mutant(h, MutantOwnerInformation, sizeof(MUTANT_OWNER_INFORMATION));
                EXPECT_EQ(STATUS_SUCCESS, result.status);
                EXPECT_EQ(sizeof(MUTANT_OWNER_INFORMATION), result.return_length);
                return emulator_object<MUTANT_OWNER_INFORMATION>{this->emu->memory, this->buffer_address()}.read().ClientId;
            }

            query_result query_semaphore(const handle h, const ULONG information_class, const ULONG length)
            {
                this->reset_outputs();
                const emulator_object<SEMAPHORE_BASIC_INFORMATION> information{this->emu->memory, this->buffer_address()};
                return this->read_outputs(syscalls::handle_NtQuerySemaphore(this->context(), h, information_class, information, length,
                                                                            this->return_length_slot()));
            }

            NTSTATUS reset_event(const handle h, LONG& previous_state)
            {
                const emulator_object<LONG> previous{this->emu->memory, this->storage + 0x300};
                previous.write(untouched_state);
                const auto status = syscalls::handle_NtResetEvent(this->context(), h, previous);
                previous_state = previous.read();
                return status;
            }

            bool is_signaled(const handle h)
            {
                return this->emu->process.events.get(h)->signaled;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };

        void expect_untouched(const query_result& result)
        {
            EXPECT_EQ(untouched_length, result.return_length);
            EXPECT_EQ(untouched_buffer(), result.bytes);
        }
    }

    TEST_F(SyncQueryTest, MutantBasicInformationReportsTheSignalStateAndOwnership)
    {
        const auto owned = this->create_mutant(true);
        auto info = this->query_mutant_basic(owned);
        EXPECT_EQ(0, info.CurrentCount);
        EXPECT_EQ(TRUE, info.OwnedByCaller);
        EXPECT_EQ(FALSE, info.AbandonedState);

        const auto thread_id = this->context().thread().id;
        ASSERT_TRUE(this->mutant_object(owned).try_lock(thread_id).has_value());
        ASSERT_TRUE(this->mutant_object(owned).try_lock(thread_id).has_value());
        info = this->query_mutant_basic(owned);
        EXPECT_EQ(-2, info.CurrentCount);
        EXPECT_EQ(TRUE, info.OwnedByCaller);

        info = this->query_mutant_basic(this->create_mutant(false));
        EXPECT_EQ(1, info.CurrentCount);
        EXPECT_EQ(FALSE, info.OwnedByCaller);
        EXPECT_EQ(FALSE, info.AbandonedState);

        const auto foreign = this->create_mutant(false);
        ASSERT_TRUE(this->mutant_object(foreign).try_lock(other_thread_id).has_value());
        info = this->query_mutant_basic(foreign);
        EXPECT_EQ(0, info.CurrentCount);
        EXPECT_EQ(FALSE, info.OwnedByCaller);
        EXPECT_EQ(FALSE, info.AbandonedState);
    }

    TEST_F(SyncQueryTest, MutantBasicInformationReportsAnAbandonedMutantAsSignaled)
    {
        const auto h = this->create_mutant(false);
        ASSERT_TRUE(this->mutant_object(h).try_lock(other_thread_id).has_value());
        this->mutant_object(h).abandon();

        auto info = this->query_mutant_basic(h);
        EXPECT_EQ(1, info.CurrentCount);
        EXPECT_EQ(FALSE, info.OwnedByCaller);
        EXPECT_EQ(TRUE, info.AbandonedState);

        ASSERT_TRUE(this->mutant_object(h).try_lock(this->context().thread().id).has_value());
        info = this->query_mutant_basic(h);
        EXPECT_EQ(0, info.CurrentCount);
        EXPECT_EQ(TRUE, info.OwnedByCaller);
        EXPECT_EQ(FALSE, info.AbandonedState);
    }

    TEST_F(SyncQueryTest, MutantOwnerInformationReportsTheOwningClientId)
    {
        const auto process_id = this->emu->process.process_id;

        auto owner = this->query_mutant_owner(this->create_mutant(true));
        EXPECT_EQ(process_id, owner.UniqueProcess);
        EXPECT_EQ(this->context().thread().id, owner.UniqueThread);

        const auto foreign = this->create_mutant(false);
        ASSERT_TRUE(this->mutant_object(foreign).try_lock(other_thread_id).has_value());
        owner = this->query_mutant_owner(foreign);
        EXPECT_EQ(process_id, owner.UniqueProcess);
        EXPECT_EQ(other_thread_id, owner.UniqueThread);

        owner = this->query_mutant_owner(this->create_mutant(false));
        EXPECT_EQ(0u, owner.UniqueProcess);
        EXPECT_EQ(0u, owner.UniqueThread);

        this->mutant_object(foreign).abandon();
        owner = this->query_mutant_owner(foreign);
        EXPECT_EQ(0u, owner.UniqueProcess);
        EXPECT_EQ(0u, owner.UniqueThread);
    }

    TEST_F(SyncQueryTest, MutantQueryRequiresTheExactLengthOfTheClass)
    {
        const auto h = this->create_mutant(true);

        for (const ULONG length : {0u, 7u, 9u, 16u})
        {
            const auto result = this->query_mutant(h, MutantBasicInformation, length);
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, result.status) << length;
            expect_untouched(result);
        }

        for (const ULONG length : {8u, 17u})
        {
            const auto result = this->query_mutant(h, MutantOwnerInformation, length);
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, result.status) << length;
            expect_untouched(result);
        }
    }

    TEST_F(SyncQueryTest, MutantQueryRejectsUnknownClassesBeforeTheHandle)
    {
        for (const auto h : {this->create_mutant(true), NULL_HANDLE})
        {
            for (const ULONG information_class : {2u, 0x7FFFu})
            {
                const auto result = this->query_mutant(h, information_class, sizeof(MUTANT_OWNER_INFORMATION));
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, result.status) << information_class;
                expect_untouched(result);
            }
        }

        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query_mutant(NULL_HANDLE, MutantBasicInformation, 4).status);
    }

    TEST_F(SyncQueryTest, MutantQueryValidatesTheHandle)
    {
        const auto closed = this->closed_handle(this->create_mutant(false));
        for (const auto h : {NULL_HANDLE, make_handle(0x1234), closed})
        {
            const auto result = this->query_mutant(h, MutantBasicInformation, sizeof(MUTANT_BASIC_INFORMATION));
            EXPECT_EQ(STATUS_INVALID_HANDLE, result.status) << h.bits;
            expect_untouched(result);
        }

        for (const auto h : {this->create_event(NotificationEvent, false), CURRENT_PROCESS, CURRENT_THREAD})
        {
            const auto result = this->query_mutant(h, MutantBasicInformation, sizeof(MUTANT_BASIC_INFORMATION));
            EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, result.status) << h.bits;
            expect_untouched(result);
        }
    }

    TEST_F(SyncQueryTest, MutantQueryAcceptsAMissingReturnLength)
    {
        EXPECT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtQueryMutant(this->context(), this->create_mutant(true), MutantBasicInformation, this->buffer_address(),
                                                 sizeof(MUTANT_BASIC_INFORMATION), {this->emu->memory}));
    }

    TEST_F(SyncQueryTest, SemaphoreBasicInformationReportsTheCurrentAndMaximumCount)
    {
        const auto h = this->create_semaphore(2, 5);
        auto result = this->query_semaphore(h, SemaphoreBasicInformation, sizeof(SEMAPHORE_BASIC_INFORMATION));
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        EXPECT_EQ(sizeof(SEMAPHORE_BASIC_INFORMATION), result.return_length);
        const emulator_object<SEMAPHORE_BASIC_INFORMATION> information{this->emu->memory, this->buffer_address()};
        EXPECT_EQ(2, information.read().CurrentCount);
        EXPECT_EQ(5, information.read().MaximumCount);
        EXPECT_EQ(untouched_byte, result.bytes[sizeof(SEMAPHORE_BASIC_INFORMATION)]);

        ASSERT_TRUE(this->emu->process.semaphores.get(h)->try_lock());
        result = this->query_semaphore(h, SemaphoreBasicInformation, sizeof(SEMAPHORE_BASIC_INFORMATION));
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        EXPECT_EQ(1, information.read().CurrentCount);
        EXPECT_EQ(5, information.read().MaximumCount);

        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtQuerySemaphore(this->context(), h, SemaphoreBasicInformation, information,
                                                                    sizeof(SEMAPHORE_BASIC_INFORMATION), {this->emu->memory}));
    }

    TEST_F(SyncQueryTest, SemaphoreQueryRequiresTheExactLength)
    {
        const auto h = this->create_semaphore(2, 5);
        for (const ULONG length : {0u, 4u, 7u, 9u, 16u})
        {
            const auto result = this->query_semaphore(h, SemaphoreBasicInformation, length);
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, result.status) << length;
            expect_untouched(result);
        }
    }

    TEST_F(SyncQueryTest, SemaphoreQueryRejectsUnknownClassesBeforeTheHandle)
    {
        for (const auto h : {this->create_semaphore(2, 5), NULL_HANDLE})
        {
            for (const ULONG information_class : {1u, 0x7FFFu})
            {
                const auto result = this->query_semaphore(h, information_class, sizeof(SEMAPHORE_BASIC_INFORMATION));
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, result.status) << information_class;
                expect_untouched(result);
            }
        }

        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query_semaphore(NULL_HANDLE, SemaphoreBasicInformation, 4).status);
    }

    TEST_F(SyncQueryTest, SemaphoreQueryValidatesTheHandle)
    {
        const auto closed = this->closed_handle(this->create_semaphore(1, 1));
        for (const auto h : {NULL_HANDLE, make_handle(0x1234), closed})
        {
            const auto result = this->query_semaphore(h, SemaphoreBasicInformation, sizeof(SEMAPHORE_BASIC_INFORMATION));
            EXPECT_EQ(STATUS_INVALID_HANDLE, result.status) << h.bits;
            expect_untouched(result);
        }

        for (const auto h : {this->create_event(NotificationEvent, false), CURRENT_PROCESS})
        {
            const auto result = this->query_semaphore(h, SemaphoreBasicInformation, sizeof(SEMAPHORE_BASIC_INFORMATION));
            EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, result.status) << h.bits;
            expect_untouched(result);
        }
    }

    TEST_F(SyncQueryTest, ResetEventReturnsThePreviousStateAndClearsTheEvent)
    {
        const auto h = this->create_event(NotificationEvent, true);

        LONG previous{};
        EXPECT_EQ(STATUS_SUCCESS, this->reset_event(h, previous));
        EXPECT_EQ(1, previous);
        EXPECT_FALSE(this->is_signaled(h));

        EXPECT_EQ(STATUS_SUCCESS, this->reset_event(h, previous));
        EXPECT_EQ(0, previous);
        EXPECT_FALSE(this->is_signaled(h));

        const auto synchronization = this->create_event(SynchronizationEvent, true);
        EXPECT_EQ(STATUS_SUCCESS, this->reset_event(synchronization, previous));
        EXPECT_EQ(1, previous);
        EXPECT_FALSE(this->is_signaled(synchronization));
    }

    TEST_F(SyncQueryTest, ResetEventAcceptsAMissingPreviousState)
    {
        const auto h = this->create_event(NotificationEvent, true);
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtResetEvent(this->context(), h, {this->emu->memory}));
        EXPECT_FALSE(this->is_signaled(h));
    }

    TEST_F(SyncQueryTest, ResetEventValidatesTheHandle)
    {
        const auto closed = this->closed_handle(this->create_event(NotificationEvent, true));
        for (const auto h : {NULL_HANDLE, make_handle(0x1234), closed})
        {
            LONG previous{};
            EXPECT_EQ(STATUS_INVALID_HANDLE, this->reset_event(h, previous)) << h.bits;
            EXPECT_EQ(untouched_state, previous) << h.bits;
        }

        for (const auto h : {this->create_semaphore(1, 1), this->create_mutant(false), CURRENT_THREAD})
        {
            LONG previous{};
            EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->reset_event(h, previous)) << h.bits;
            EXPECT_EQ(untouched_state, previous) << h.bits;
        }
    }
}
