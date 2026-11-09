#include "emulation_test_utils.hpp"

#include <snapshot.hpp>
#include <syscall_utils.hpp>
#include <utils/io.hpp>

#include <chrono>
#include <filesystem>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateTimer(const syscall_context& c, emulator_object<handle> timer_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG timer_type);
    NTSTATUS handle_NtSetTimer(const syscall_context& c, handle timer_handle, emulator_object<LARGE_INTEGER> due_time, uint64_t apc_routine,
                               uint64_t apc_context, BOOLEAN resume_timer, LONG period, emulator_object<BOOLEAN> previous_state);
}

namespace sogen::test
{
    namespace
    {
        void dump_and_expect_equal(const char* label, const std::vector<std::byte>& a, const std::vector<std::byte>& b)
        {
            if (a == b)
            {
                return;
            }

            utils::io::write_file(std::string(label) + ".a.bin", a);
            utils::io::write_file(std::string(label) + ".b.bin", b);

            ADD_FAILURE() << label << ": serialized buffers differ (dumps written to the working directory)";
        }
    }

    TEST(SerializationTest, ResettingEmulatorWorks)
    {
        auto emu = create_sample_emulator();

        utils::buffer_serializer start_state{};
        emu.serialize(start_state);

        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer end_state1{};
        emu.serialize(end_state1);

        utils::buffer_deserializer deserializer{start_state};
        emu.deserialize(deserializer);

        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer end_state2{};
        emu.serialize(end_state2);

        dump_and_expect_equal("ResettingEmulatorWorks", end_state1.get_buffer(), end_state2.get_buffer());
    }

    // Each test keeps a single emulator alive at a time: backends that place guest memory at its host address can only
    // have one per process.

    TEST(SerializationTest, SerializedDataIsReproducible)
    {
        utils::buffer_serializer serializer1{};

        {
            auto emu1 = create_sample_emulator();
            emu1.start();

            ASSERT_TERMINATED_SUCCESSFULLY(emu1);

            emu1.serialize(serializer1);
        }

        utils::buffer_deserializer deserializer{serializer1};

        auto new_emu = create_empty_emulator();
        new_emu.deserialize(deserializer);

        utils::buffer_serializer serializer2{};
        new_emu.serialize(serializer2);

        dump_and_expect_equal("SerializedDataIsReproducible", serializer1.get_buffer(), serializer2.get_buffer());
    }

    TEST(SerializationTest, EmulationIsReproducible)
    {
        utils::buffer_serializer serializer1{};

        {
            auto emu1 = create_sample_emulator();
            emu1.start();

            ASSERT_TERMINATED_SUCCESSFULLY(emu1);

            emu1.serialize(serializer1);
        }

        auto emu2 = create_sample_emulator();
        emu2.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu2);

        utils::buffer_serializer serializer2{};
        emu2.serialize(serializer2);

        dump_and_expect_equal("EmulationIsReproducible", serializer1.get_buffer(), serializer2.get_buffer());
    }

    TEST(SerializationTest, DeserializedEmulatorBehavesLikeSource)
    {
        utils::buffer_serializer serializer{};
        utils::buffer_serializer serializer1{};

        {
            auto emu = create_sample_emulator();
            emu.start(100);

            emu.serialize(serializer);

            emu.start();
            ASSERT_TERMINATED_SUCCESSFULLY(emu);

            emu.serialize(serializer1);
        }

        utils::buffer_deserializer deserializer{serializer};

        auto new_emu = create_empty_emulator();
        new_emu.deserialize(deserializer);

        new_emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);

        utils::buffer_serializer serializer2{};
        new_emu.serialize(serializer2);

        dump_and_expect_equal("DeserializedEmulatorBehavesLikeSource", serializer1.get_buffer(), serializer2.get_buffer());
    }

    namespace
    {
        // A host clock that moves only when the test moves it; its counter runs at 1 GHz.
        class manual_clock : public utils::clock
        {
          public:
            system_time_point system_now() override
            {
                return this->system;
            }

            steady_time_point steady_now() override
            {
                return this->steady;
            }

            uint64_t timestamp_counter() override
            {
                const auto value = this->counter;
                this->advance(std::exchange(this->after_next_counter_read, {}));
                return value;
            }

            void advance(const std::chrono::nanoseconds time)
            {
                this->system += std::chrono::duration_cast<system_duration>(time);
                this->steady += time;
                this->counter += static_cast<uint64_t>(time.count());
            }

            system_time_point system{std::chrono::seconds(1'700'000'000)};
            steady_time_point steady{std::chrono::hours(1)};
            uint64_t counter{1'000'000'000'000};
            // Time that passes once the counter is read, the last of the guest's clocks a snapshot reads: a snapshot taken
            // then takes this long.
            std::chrono::nanoseconds after_next_counter_read{};
        };

        // The KUSD is a page refreshed from the clock, as with --native-timing.
        windows_emulator create_clocked_emulator(manual_clock*& clock, const bool sample = true, const bool keep_guest_time = true)
        {
            emulator_settings settings{};
            settings.use_relative_time = false;
            settings.intercept_kusd = false;
            settings.keep_guest_time_across_snapshots = keep_guest_time;

            auto owned = std::make_unique<manual_clock>();
            clock = owned.get();
            emulator_interfaces interfaces{};
            interfaces.clock = std::move(owned);
            if (sample)
            {
                return create_sample_emulator(settings, {}, {}, std::move(interfaces));
            }

            return create_emulator(settings, {}, std::move(interfaces));
        }

        // The host's clocks some time after the save.
        void move_past_the_save(manual_clock& clock, const windows_emulator::guest_time& saved, const std::chrono::nanoseconds time)
        {
            clock.steady = saved.steady;
            clock.system = saved.system;
            clock.counter = saved.timestamp_counter;
            clock.advance(time);
        }

        uint64_t interrupt_time(windows_emulator& emu)
        {
            emu.process.kusd.refresh();
            return emu.memory.read_memory<uint64_t>(kusd_mmio::address() + offsetof(KUSER_SHARED_DATA64, InterruptTime));
        }

        uint64_t hundred_nanoseconds(const utils::clock::steady_time_point time)
        {
            return std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<1, 10'000'000>>>(time.time_since_epoch()).count();
        }

        // A notification timer due at `due`, in 100 ns units: an absolute system time when positive, an interval when not.
        handle set_timer(windows_emulator& emu, const int64_t due)
        {
            const syscall_context c{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
            const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            const emulator_object<handle> timer{emu.memory, storage};
            const emulator_object<LARGE_INTEGER> due_time{emu.memory, storage + 0x10};
            due_time.write({.QuadPart = due});
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateTimer(c, timer, 0, {emu.memory}, 0));
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetTimer(c, timer.read(), due_time, 0, 0, FALSE, 0, {emu.memory}));
            return timer.read();
        }

        int64_t windows_time(const utils::clock::system_time_point time)
        {
            return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(time.time_since_epoch()).count() +
                   WINDOWS_EPOCH_DIFFERENCE;
        }

        bool is_signaled(windows_emulator& emu, const handle timer)
        {
            return emu.process.timers.get(timer)->is_signaled(emu.clock());
        }
    }

    // A load continues the guest's time from the save, not from the time that passed on the host until the load: its
    // interrupt time, which timeGetTime reads (19041 kernel32 ReadInterruptTick, KUSD 0x7FFE0008), and its timestamp
    // counter go on from the save, as a Hyper-V partition's time stands still while it is in a saved state and a restored
    // partition's TSC continues. Destiny 2's activity host timeout (20 s) runs on timeGetTime, so a jump removes the player
    // from the world after a load.
    TEST(SerializationTest, RestoreContinuesTheTimeOfTheSave)
    {
        utils::buffer_serializer serializer{};
        windows_emulator::guest_time saved{};
        {
            manual_clock* clock{};
            auto emu = create_clocked_emulator(clock);
            emu.setup_process_if_necessary();
            saved = emu.current_guest_time();
            emu.serialize(serializer);
        }

        manual_clock* clock{};
        auto restored = create_clocked_emulator(clock, false);
        move_past_the_save(*clock, saved, std::chrono::seconds(350));
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        EXPECT_EQ(saved.steady, restored.clock().steady_now());
        EXPECT_EQ(saved.timestamp_counter, restored.clock().timestamp_counter());
        EXPECT_EQ(hundred_nanoseconds(saved.steady), interrupt_time(restored));

        clock->advance(std::chrono::milliseconds(10));
        EXPECT_EQ(saved.steady + std::chrono::milliseconds(10), restored.clock().steady_now());
        EXPECT_EQ(saved.timestamp_counter + 10'000'000, restored.clock().timestamp_counter());
    }

    // Without the option, a load takes the host's time, as before snapshots could keep the guest's.
    TEST(SerializationTest, RestoreTakesTheHostsTimeWithoutKeepingTheGuestTime)
    {
        utils::buffer_serializer serializer{};
        windows_emulator::guest_time saved{};
        {
            manual_clock* clock{};
            auto emu = create_clocked_emulator(clock, true, false);
            emu.setup_process_if_necessary();
            saved = emu.current_guest_time();
            emu.serialize(serializer);
        }

        manual_clock* clock{};
        auto restored = create_clocked_emulator(clock, false, false);
        move_past_the_save(*clock, saved, std::chrono::seconds(350));
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        EXPECT_EQ(saved.steady + std::chrono::seconds(350), restored.clock().steady_now());
        EXPECT_EQ(saved.timestamp_counter + 350'000'000'000, restored.clock().timestamp_counter());
    }

    // A process that keeps running after a save does not see the time the snapshot took to write (37 s for Destiny 2 in a
    // world, past its 20 s activity host timeout): its time goes on from when it was saved.
    TEST(SerializationTest, WritingASnapshotTakesNoTimeFromTheProcess)
    {
        manual_clock* clock{};
        auto emu = create_clocked_emulator(clock);
        emu.setup_process_if_necessary();

        const auto file = std::filesystem::temp_directory_path() / ("sogen-snapshot-time-" + std::to_string(getpid()) + ".snap");
        const auto before = emu.current_guest_time();
        // should_commit runs after the state is written, before the file is moved into place.
        snapshot::write_emulator_snapshot(emu, file, {}, [&] {
            clock->advance(std::chrono::seconds(37));
            return true;
        });
        std::filesystem::remove(file);

        EXPECT_EQ(before.steady, emu.clock().steady_now());
        EXPECT_EQ(before.timestamp_counter, emu.clock().timestamp_counter());
    }

    TEST(SerializationTest, TakingASnapshotInMemoryTakesNoTimeFromTheProcess)
    {
        manual_clock* clock{};
        auto emu = create_clocked_emulator(clock);
        emu.setup_process_if_necessary();

        const auto before = emu.current_guest_time();
        clock->after_next_counter_read = std::chrono::seconds(37);
        (void)snapshot::create_emulator_snapshot(emu, {});

        EXPECT_EQ(before.steady, emu.clock().steady_now());
        EXPECT_EQ(before.timestamp_counter, emu.clock().timestamp_counter());
    }

    // save_snapshot and restore_snapshot (the fuzzer, the Python bindings) keep the time as a snapshot file does.
    TEST(SerializationTest, RestoringASnapshotInMemoryContinuesTheTimeOfTheSave)
    {
        manual_clock* clock{};
        auto emu = create_clocked_emulator(clock);
        emu.setup_process_if_necessary();
        const auto relative = set_timer(emu, -2'000'000);

        const auto saved = emu.current_guest_time();
        emu.save_snapshot();
        clock->advance(std::chrono::seconds(1));
        emu.restore_snapshot();

        EXPECT_EQ(saved.steady, emu.clock().steady_now());
        EXPECT_EQ(saved.timestamp_counter, emu.clock().timestamp_counter());
        EXPECT_FALSE(is_signaled(emu, relative));
    }

    // A timer due at an absolute system time follows the system time, as on Windows when the system time changes: one due
    // while the snapshot was on disk is signaled after the load, where a timer due after an interval still waits it out.
    TEST(SerializationTest, AbsoluteTimerDueWhileTheSnapshotWasOnDiskIsSignaledAfterTheLoad)
    {
        utils::buffer_serializer serializer{};
        windows_emulator::guest_time saved{};
        handle absolute{};
        handle relative{};
        {
            manual_clock* clock{};
            auto emu = create_clocked_emulator(clock);
            emu.setup_process_if_necessary();
            absolute = set_timer(emu, windows_time(clock->system) + 2'000'000);
            relative = set_timer(emu, -2'000'000);
            saved = emu.current_guest_time();
            emu.serialize(serializer);
        }

        manual_clock* clock{};
        auto restored = create_clocked_emulator(clock, false);
        move_past_the_save(*clock, saved, std::chrono::seconds(1));
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        EXPECT_TRUE(is_signaled(restored, absolute));
        EXPECT_FALSE(is_signaled(restored, relative));
        clock->advance(std::chrono::milliseconds(200));
        EXPECT_TRUE(is_signaled(restored, relative));
    }

    // On a host whose clock is behind the save, an absolute timer still waits for its system time.
    TEST(SerializationTest, AbsoluteTimerWaitsForItsSystemTimeOnAHostWhoseClockIsBehind)
    {
        utils::buffer_serializer serializer{};
        windows_emulator::guest_time saved{};
        handle absolute{};
        handle relative{};
        {
            manual_clock* clock{};
            auto emu = create_clocked_emulator(clock);
            emu.setup_process_if_necessary();
            absolute = set_timer(emu, windows_time(clock->system) + 2'000'000);
            relative = set_timer(emu, -2'000'000);
            saved = emu.current_guest_time();
            emu.serialize(serializer);
        }

        manual_clock* clock{};
        auto restored = create_clocked_emulator(clock, false);
        move_past_the_save(*clock, saved, {});
        clock->system -= std::chrono::seconds(1);
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        clock->advance(std::chrono::milliseconds(200));
        EXPECT_TRUE(is_signaled(restored, relative));
        EXPECT_FALSE(is_signaled(restored, absolute));
        clock->advance(std::chrono::seconds(1));
        EXPECT_TRUE(is_signaled(restored, absolute));
    }
} // namespace sogen::test
