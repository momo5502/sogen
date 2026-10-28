#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtQueryEvent(const syscall_context& c, handle event_handle, uint32_t event_information_class,
                                 emulator_object<EVENT_BASIC_INFORMATION> event_information, uint32_t event_information_length,
                                 emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, LONG initial_count,
                                      LONG maximum_count);
    NTSTATUS handle_NtReleaseSemaphore(const syscall_context& c, handle semaphore_handle, LONG release_count,
                                       emulator_object<LONG> previous_count);
    NTSTATUS handle_NtWaitForMultipleObjects(const syscall_context& c, ULONG count, emulator_object<handle> handles, WAIT_TYPE wait_type,
                                             BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtWaitForMultipleObjects32(const syscall_context& c, ULONG count, emulator_object<uint32_t> handles,
                                               WAIT_TYPE wait_type, BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
}

namespace sogen::test
{
    namespace
    {
        constexpr LONG untouched = 0x55555555;

        class SyncArgumentValidationTest : public testing::Test
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

            NTSTATUS create_event(const EVENT_TYPE type, handle* result = nullptr)
            {
                const auto status =
                    syscalls::handle_NtCreateEvent(this->context(), this->handle_slot(), 0, {this->emu->memory}, type, FALSE);
                if (result)
                {
                    *result = this->handle_slot().read();
                }
                return status;
            }

            NTSTATUS create_semaphore(const LONG initial_count, const LONG maximum_count, handle* result = nullptr)
            {
                const auto status = syscalls::handle_NtCreateSemaphore(this->context(), this->handle_slot(), 0, {this->emu->memory},
                                                                       initial_count, maximum_count);
                if (result)
                {
                    *result = this->handle_slot().read();
                }
                return status;
            }

            NTSTATUS release_semaphore(const handle semaphore, const LONG count, LONG& previous_count)
            {
                const emulator_object<LONG> previous{this->emu->memory, this->storage + 0x100};
                previous.write(untouched);
                const auto status = syscalls::handle_NtReleaseSemaphore(this->context(), semaphore, count, previous);
                previous_count = previous.read();
                return status;
            }

            NTSTATUS query_event(const handle event, const uint32_t length, uint32_t& return_length)
            {
                const emulator_object<EVENT_BASIC_INFORMATION> information{this->emu->memory, this->storage + 0x200};
                const emulator_object<uint32_t> returned{this->emu->memory, this->storage + 0x300};
                returned.write(0xAAAAAAAA);
                const auto status = syscalls::handle_NtQueryEvent(this->context(), event, 0, information, length, returned);
                return_length = returned.read();
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(SyncArgumentValidationTest, CreateEventRejectsUnknownEventTypes)
    {
        EXPECT_EQ(STATUS_SUCCESS, this->create_event(NotificationEvent));
        EXPECT_EQ(STATUS_SUCCESS, this->create_event(SynchronizationEvent));

        for (const auto type : {2, 3, -1})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create_event(static_cast<EVENT_TYPE>(type))) << type;
        }
    }

    TEST_F(SyncArgumentValidationTest, QueryEventRequiresTheExactInformationLength)
    {
        handle event{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_event(SynchronizationEvent, &event));

        uint32_t return_length{};
        EXPECT_EQ(STATUS_SUCCESS, this->query_event(event, sizeof(EVENT_BASIC_INFORMATION), return_length));
        EXPECT_EQ(sizeof(EVENT_BASIC_INFORMATION), return_length);

        for (const uint32_t length : {0u, 4u, 7u, 9u, 16u})
        {
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query_event(event, length, return_length)) << length;
            EXPECT_EQ(0xAAAAAAAAu, return_length) << length;
        }
    }

    TEST_F(SyncArgumentValidationTest, CreateSemaphoreRejectsSignedCountsOutOfRange)
    {
        EXPECT_EQ(STATUS_SUCCESS, this->create_semaphore(0, 1));
        EXPECT_EQ(STATUS_SUCCESS, this->create_semaphore(1, 1));
        EXPECT_EQ(STATUS_SUCCESS, this->create_semaphore(INT32_MAX, INT32_MAX));

        struct counts
        {
            LONG initial;
            LONG maximum;
        };

        for (const auto [initial, maximum] :
             {counts{2, 1}, counts{-1, 5}, counts{0, 0}, counts{0, -1}, counts{0, INT32_MIN}, counts{INT32_MIN, 5}})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create_semaphore(initial, maximum)) << initial << " " << maximum;
        }
    }

    TEST_F(SyncArgumentValidationTest, ReleaseSemaphoreRejectsNonPositiveCounts)
    {
        handle semaphore{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_semaphore(0, 3, &semaphore));

        LONG previous{};
        for (const LONG count : {0, -1, INT32_MIN})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->release_semaphore(semaphore, count, previous)) << count;
            EXPECT_EQ(untouched, previous) << count;
        }

        EXPECT_EQ(STATUS_SUCCESS, this->release_semaphore(semaphore, 1, previous));
        EXPECT_EQ(0, previous);
    }

    TEST_F(SyncArgumentValidationTest, ReleaseSemaphoreBeyondTheLimitLeavesThePreviousCountAlone)
    {
        handle semaphore{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_semaphore(2, 3, &semaphore));

        LONG previous{};
        EXPECT_EQ(STATUS_SEMAPHORE_LIMIT_EXCEEDED, this->release_semaphore(semaphore, 2, previous));
        EXPECT_EQ(untouched, previous);

        EXPECT_EQ(STATUS_SUCCESS, this->release_semaphore(semaphore, 1, previous));
        EXPECT_EQ(2, previous);
    }

    TEST_F(SyncArgumentValidationTest, ReleaseSemaphoreRefusesAnotherObjectType)
    {
        handle event{};
        ASSERT_EQ(STATUS_SUCCESS, this->create_event(NotificationEvent, &event));

        LONG previous{};
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->release_semaphore(event, 1, previous));
        EXPECT_EQ(untouched, previous);
    }

    TEST_F(SyncArgumentValidationTest, WaitForMultipleObjectsChecksTheCountBeforeTheWaitType)
    {
        const emulator_object<handle> handles{this->emu->memory, this->storage + 0x400};
        const emulator_object<uint32_t> handles32{this->emu->memory, this->storage + 0x400};
        const emulator_object<LARGE_INTEGER> timeout{this->emu->memory, this->storage + 0x500};
        timeout.write(LARGE_INTEGER{});

        for (const auto type : {WaitAll, WaitAny, static_cast<WAIT_TYPE>(2), static_cast<WAIT_TYPE>(-1)})
        {
            for (const ULONG count : {0u, 65u, 0xFFFFFFFFu})
            {
                EXPECT_EQ(STATUS_INVALID_PARAMETER_1,
                          syscalls::handle_NtWaitForMultipleObjects(this->context(), count, handles, type, FALSE, timeout))
                    << count << " " << type;
                EXPECT_EQ(STATUS_INVALID_PARAMETER_1,
                          syscalls::handle_NtWaitForMultipleObjects32(this->context(), count, handles32, type, FALSE, timeout))
                    << count << " " << type;
            }
        }
    }

    TEST_F(SyncArgumentValidationTest, WaitForMultipleObjectsRejectsAnUnknownWaitType)
    {
        const emulator_object<handle> handles{this->emu->memory, this->storage + 0x400};
        const emulator_object<uint32_t> handles32{this->emu->memory, this->storage + 0x400};
        const emulator_object<LARGE_INTEGER> timeout{this->emu->memory, this->storage + 0x500};
        timeout.write(LARGE_INTEGER{});

        for (const ULONG count : {1u, 64u})
        {
            for (const auto type : {static_cast<WAIT_TYPE>(2), static_cast<WAIT_TYPE>(-1)})
            {
                EXPECT_EQ(STATUS_INVALID_PARAMETER_3,
                          syscalls::handle_NtWaitForMultipleObjects(this->context(), count, handles, type, FALSE, timeout))
                    << count << " " << type;
                EXPECT_EQ(STATUS_INVALID_PARAMETER_3,
                          syscalls::handle_NtWaitForMultipleObjects32(this->context(), count, handles32, type, FALSE, timeout))
                    << count << " " << type;
            }
        }
    }
}
