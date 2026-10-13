#include "emulation_test_utils.hpp"
#include <utils/finally.hpp>

#include <future>

namespace sogen::test
{
    namespace
    {
        class idle_ui_backend final : public ui_backend
        {
          public:
            std::function<void()> on_pump{};
            size_t polls{};

            void set_event_sink(event_sink) override
            {
            }

            void pump_events() override
            {
                ++this->polls;
                if (this->on_pump)
                {
                    this->on_pump();
                }
            }
        };

        class IdleSchedulerTest : public testing::Test
        {
          protected:
            void create(bool relative_time = false)
            {
                emulator_interfaces interfaces{};
                auto backend = std::make_unique<idle_ui_backend>();
                this->ui = backend.get();
                interfaces.ui = std::move(backend);
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true,
                                                                                 .use_relative_time = relative_time,
                                                                                 .emulation_root = get_emulator_root(),
                                                                                 .load_registry = false},
                                                               emulator_callbacks{}, std::move(interfaces));
                const auto handle =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->thread = this->emu->process.threads.get(handle);
                this->emu->vcpu(0).active_thread = this->thread;
                this->thread->setup_if_necessary(this->emu->vcpu(0).cpu, this->emu->process);
            }

            void stop_after(std::chrono::milliseconds duration)
            {
                const auto deadline = std::chrono::steady_clock::now() + duration;
                this->ui->on_pump = [this, deadline] {
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        this->emu->stop();
                    }
                };
            }

            std::unique_ptr<windows_emulator> emu{};
            idle_ui_backend* ui{};
            emulator_thread* thread{};
        };
    }

    TEST_F(IdleSchedulerTest, PendingFileIoDoesNotBusyPoll)
    {
        this->create();
        const auto storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>{this->emu->memory, storage}.write(
            {.Status = STATUS_PENDING, .Information = 0});
        this->thread->await_file_io = storage;
        this->stop_after(std::chrono::milliseconds(50));
        EXPECT_FALSE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_GT(this->ui->polls, 0u);
        EXPECT_LT(this->ui->polls, 1000u);
        EXPECT_EQ(this->thread->await_file_io, storage);
    }

    TEST_F(IdleSchedulerTest, HostConditionMakesParkedThreadRunnable)
    {
        this->create();
        size_t polls{};
        this->thread->await_host_condition = [&polls] { return ++polls == 3; };
        EXPECT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_EQ(polls, 3u);
        EXPECT_FALSE(this->thread->await_host_condition);
        EXPECT_EQ(STATUS_SUCCESS, this->emu->vcpu(0).cpu.reg<uint64_t>(x86_register::rax));
    }

    TEST_F(IdleSchedulerTest, FileCompletionMakesParkedThreadRunnable)
    {
        this->create();
        const auto storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status{this->emu->memory, storage};
        status.write({.Status = STATUS_PENDING, .Information = 0});
        this->thread->await_file_io = storage;
        this->ui->on_pump = [status] { status.write({.Status = STATUS_SUCCESS, .Information = 0}); };
        EXPECT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_FALSE(this->thread->await_file_io.has_value());
        EXPECT_EQ(STATUS_SUCCESS, this->emu->vcpu(0).cpu.reg<uint64_t>(x86_register::rax));
    }

    TEST_F(IdleSchedulerTest, TimerMakesParkedThreadRunnable)
    {
        this->create();
        const auto deadline = this->emu->clock().steady_now() + std::chrono::milliseconds(20);
        this->thread->await_time = deadline;
        EXPECT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_GE(this->emu->clock().steady_now(), deadline);
        EXPECT_FALSE(this->thread->await_time.has_value());
    }

    TEST_F(IdleSchedulerTest, RelativeTimeStillAdvancesWhileHostConditionIsPending)
    {
        this->create(true);
        const auto before = this->emu->clock().steady_now();
        this->thread->await_host_condition = [this, before] { return this->emu->clock().steady_now() > before; };
        EXPECT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_GT(this->emu->get_executed_instructions(), 0u);
        EXPECT_FALSE(this->thread->await_host_condition);
    }

    TEST_F(IdleSchedulerTest, StopEndsParkedFileWait)
    {
#ifdef __EMSCRIPTEN__
        GTEST_SKIP() << "Threadless WebAssembly does not support cross-thread stop";
#else
        this->create();
        if (!this->emu->emu().is_stop_thread_safe())
        {
            GTEST_SKIP() << "Backend does not support cross-thread stop";
        }
        const auto storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>{this->emu->memory, storage}.write(
            {.Status = STATUS_PENDING, .Information = 0});
        this->thread->await_file_io = storage;
        std::promise<void> parked;
        auto parked_future = parked.get_future();
        this->ui->on_pump = [this, &parked] {
            if (this->ui->polls == 1)
            {
                parked.set_value();
            }
        };
        auto result = std::async(std::launch::async, [this] { return this->emu->perform_thread_switch(this->emu->vcpu(0)); });
        const auto cleanup = utils::finally([this] { this->emu->stop(); });
        ASSERT_EQ(std::future_status::ready, parked_future.wait_for(std::chrono::seconds(2)));
        this->emu->stop();
        ASSERT_EQ(std::future_status::ready, result.wait_for(std::chrono::seconds(2)));
        EXPECT_FALSE(result.get());
#endif
    }
}
