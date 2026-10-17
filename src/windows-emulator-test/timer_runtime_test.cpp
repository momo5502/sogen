#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateTimer2(const syscall_context& c, emulator_object<handle> timer_handle, uint64_t reserved1, uint64_t reserved2,
                                   ULONG attributes, ACCESS_MASK desired_access);
    NTSTATUS handle_NtCreateTimer(const syscall_context& c, emulator_object<handle> timer_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG timer_type);
    NTSTATUS handle_NtSetTimer(const syscall_context& c, handle timer_handle, emulator_object<LARGE_INTEGER> due_time, uint64_t apc_routine,
                               uint64_t apc_context, BOOLEAN resume_timer, LONG period, emulator_object<BOOLEAN> previous_state);
    NTSTATUS handle_NtSetTimer2(const syscall_context& c, handle timer_handle, emulator_object<LARGE_INTEGER> due_time,
                                emulator_object<LARGE_INTEGER> period, uint64_t parameters);
    NTSTATUS handle_NtCancelTimer(const syscall_context& c, handle timer_handle, emulator_object<BOOLEAN> current_state);
    NTSTATUS handle_NtCancelTimer2(const syscall_context& c, handle timer_handle, uint64_t parameters);
}

namespace sogen::test
{
    namespace
    {
        class timer_test_clock : public utils::clock
        {
          public:
            system_time_point system_now() override
            {
                return std::chrono::time_point_cast<system_duration>(system_time_point(std::chrono::seconds(1700000000)) + this->elapsed);
            }

            steady_time_point steady_now() override
            {
                return steady_time_point{} + this->elapsed;
            }

            std::chrono::nanoseconds elapsed{};
        };

        class TimerRuntimeTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                auto test_clock = std::make_unique<timer_test_clock>();
                this->clock = test_clock.get();
                interfaces.clock = std::move(test_clock);
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
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

            handle create(bool extended, ULONG type = 0)
            {
                const emulator_object<handle> out{this->emu->memory, this->storage};
                EXPECT_EQ(STATUS_SUCCESS, extended ? syscalls::handle_NtCreateTimer2(this->context(), out, 0, 0, type, 0)
                                                   : syscalls::handle_NtCreateTimer(this->context(), out, 0, {this->emu->memory}, type));
                return out.read();
            }

            NTSTATUS set(handle h, int64_t due_ticks, int64_t period_ticks = 0)
            {
                const emulator_object<LARGE_INTEGER> due{this->emu->memory, this->storage + 0x100};
                const emulator_object<LARGE_INTEGER> period{this->emu->memory, this->storage + 0x108};
                due.write({.QuadPart = due_ticks});
                period.write({.QuadPart = period_ticks});
                return syscalls::handle_NtSetTimer2(this->context(), h, due, period, this->storage + 0x200);
            }

            std::unique_ptr<windows_emulator> emu{};
            timer_test_clock* clock{};
            uint64_t storage{};
        };
    }

    TEST_F(TimerRuntimeTest, ExtendedTimerSignalsOnceAfterRelativeDeadline)
    {
        const auto h = this->create(true);
        auto* entry = this->emu->process.timers.get(h);
        ASSERT_NE(nullptr, entry);
        EXPECT_FALSE(entry->is_signaled(this->clock->steady_now()));
        ASSERT_EQ(STATUS_SUCCESS, this->set(h, -10000));
        this->clock->elapsed = std::chrono::microseconds(999);
        EXPECT_FALSE(entry->is_signaled(this->clock->steady_now()));
        this->clock->elapsed = std::chrono::milliseconds(1);
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
        entry->consume_signal();
        EXPECT_FALSE(entry->is_signaled(this->clock->steady_now()));
        EXPECT_FALSE(entry->due_time.has_value());
    }

    TEST_F(TimerRuntimeTest, CancellationPreventsPendingAndPeriodicSignals)
    {
        const auto h = this->create(true);
        auto* entry = this->emu->process.timers.get(h);
        ASSERT_EQ(STATUS_SUCCESS, this->set(h, -10000, 20000));
        this->clock->elapsed = std::chrono::milliseconds(1);
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
        entry->consume_signal();
        this->clock->elapsed = std::chrono::milliseconds(3);
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
        entry->consume_signal();
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelTimer2(this->context(), h, 0));
        this->clock->elapsed = std::chrono::seconds(1);
        EXPECT_FALSE(entry->is_signaled(this->clock->steady_now()));
        EXPECT_FALSE(entry->due_time.has_value());
        ASSERT_EQ(STATUS_SUCCESS, this->set(h, -10000));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelTimer2(this->context(), h, 0));
        this->clock->elapsed += std::chrono::seconds(1);
        EXPECT_FALSE(entry->is_signaled(this->clock->steady_now()));
    }

    TEST_F(TimerRuntimeTest, CancellationDoesNotClearAnAlreadySignaledNotificationTimer)
    {
        const auto h = this->create(false);
        const emulator_object<LARGE_INTEGER> due{this->emu->memory, this->storage + 0x100};
        due.write({.QuadPart = -1});
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetTimer(this->context(), h, due, 0, 0, FALSE, 0, {this->emu->memory}));
        this->clock->elapsed = std::chrono::milliseconds(1);
        auto* entry = this->emu->process.timers.get(h);
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
        entry->consume_signal();
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelTimer(this->context(), h, {this->emu->memory, this->storage + 0x200}));
        EXPECT_EQ(1, (emulator_object<BOOLEAN>{this->emu->memory, this->storage + 0x200}.read()));
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
    }

    TEST_F(TimerRuntimeTest, RejectsWrongTimerKindAndNegativePeriod)
    {
        const auto normal = this->create(false);
        const auto extended = this->create(true);
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->set(normal, -1));
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, syscalls::handle_NtCancelTimer2(this->context(), normal, 0));
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, syscalls::handle_NtCancelTimer(this->context(), extended, {this->emu->memory}));
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtCancelTimer2(this->context(), {}, 0));
        EXPECT_EQ(STATUS_INVALID_PARAMETER_3, this->set(extended, -1, -1));
    }

    TEST_F(TimerRuntimeTest, HugeRelativeDeadlineDoesNotOverflow)
    {
        const auto h = this->create(true);
        ASSERT_EQ(STATUS_SUCCESS, this->set(h, std::numeric_limits<int64_t>::min()));
        const auto* entry = this->emu->process.timers.get(h);
        ASSERT_TRUE(entry->due_time.has_value());
        EXPECT_EQ(std::chrono::steady_clock::time_point::max(), *entry->due_time);
    }

    TEST_F(TimerRuntimeTest, SerializationPreservesTimerSchedule)
    {
        const auto h = this->create(true);
        ASSERT_EQ(STATUS_SUCCESS, this->set(h, -10000, 20000));
        utils::buffer_serializer buffer{};
        this->emu->process.timers.serialize(buffer);
        utils::buffer_deserializer reader{buffer};
        this->emu->process.timers.deserialize(reader);
        auto* entry = this->emu->process.timers.get(h);
        EXPECT_TRUE(entry->extended);
        EXPECT_TRUE(entry->synchronization);
        EXPECT_EQ(2000000, entry->period_nanoseconds);
        this->clock->elapsed = std::chrono::milliseconds(1);
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
    }

    TEST_F(TimerRuntimeTest, LargePeriodicIntervalSaturatesInsteadOfWrapping)
    {
        const auto h = this->create(true);
        auto* entry = this->emu->process.timers.get(h);
        entry->due_time = this->clock->steady_now();
        entry->period_nanoseconds = std::numeric_limits<int64_t>::max();
        this->clock->elapsed = std::chrono::seconds(1);
        EXPECT_TRUE(entry->is_signaled(this->clock->steady_now()));
        ASSERT_TRUE(entry->due_time.has_value());
        EXPECT_EQ(std::chrono::steady_clock::time_point::max(), *entry->due_time);
    }
}
