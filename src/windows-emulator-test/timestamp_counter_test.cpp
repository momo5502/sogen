#include <backend_selection.hpp>
#include <windows_emulator.hpp>

#include <array>
#include <gtest/gtest.h>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t fixed_timestamp = 0x123456789abcdef0;

        class fixed_timestamp_clock final : public utils::clock
        {
          public:
            uint64_t timestamp_counter() override
            {
                return fixed_timestamp;
            }
        };

        class TimestampCounterTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                this->backend = create_x86_64_emulator_from_environment();
                const auto instruction = GetParam() ? x86_hookable_instructions::rdtscp : x86_hookable_instructions::rdtsc;
                auto* hook = this->backend->hook_instruction(instruction, [] { return instruction_hook_continuation::run_instruction; });
                if (!hook)
                {
                    GTEST_SKIP() << "Backend does not support timestamp instruction hooks";
                }
                this->backend->delete_hook(hook);
            }

            uint64_t execute(const bool relative_time, std::unique_ptr<utils::clock> clock = {})
            {
                auto& cpu = *this->backend;
                emulator_interfaces interfaces{};
                interfaces.clock = std::move(clock);
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                size_t callbacks = 0;
                utils::clock* active_clock = nullptr;
                uint64_t expected_timestamp = 0;
                emulator_callbacks observers{};
                const auto stop = [&] {
                    ++callbacks;
                    if (relative_time)
                    {
                        expected_timestamp = active_clock->timestamp_counter();
                    }
                    cpu.stop();
                };
                observers.on_rdtsc = stop;
                observers.on_rdtscp = stop;
                windows_emulator emu{std::move(this->backend),
                                     emulator_settings{.disable_logging = true, .use_relative_time = relative_time, .load_registry = false},
                                     std::move(observers), std::move(interfaces)};
                active_clock = &emu.clock();
                emulator_thread thread{emu.memory, emu.process, 0, 0, 0x100000, 0, 8, true};
                emu.vcpu(0).active_thread = &thread;
                const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
                const std::array<uint8_t, 3> rdtscp{0x0f, 0x01, 0xf9};
                const std::array<uint8_t, 2> rdtsc{0x0f, 0x31};
                if (GetParam())
                {
                    emu.memory.write_memory(code, rdtscp.data(), rdtscp.size());
                }
                else
                {
                    emu.memory.write_memory(code, rdtsc.data(), rdtsc.size());
                }
                cpu.reg(x86_register::rip, code);
                cpu.reg(x86_register::rax, UINT64_MAX);
                cpu.reg(x86_register::rdx, UINT64_MAX);
                cpu.start();
                EXPECT_EQ(callbacks, 1u);
                EXPECT_LE(cpu.reg(x86_register::rax), UINT32_MAX);
                EXPECT_LE(cpu.reg(x86_register::rdx), UINT32_MAX);
                const auto timestamp = cpu.reg(x86_register::rax) | (cpu.reg(x86_register::rdx) << 32);
                if (relative_time)
                {
                    EXPECT_EQ(timestamp, expected_timestamp);
                }
                return timestamp;
            }

            std::unique_ptr<x86_64_emulator> backend{};
        };
    }

    TEST_P(TimestampCounterTest, NativeClockProducesTimestampAndNotifiesObservers)
    {
        EXPECT_GT(execute(false), 0u);
    }

    TEST_P(TimestampCounterTest, InjectedClockOverridesNativeTimestamp)
    {
        EXPECT_EQ(execute(false, std::make_unique<fixed_timestamp_clock>()), fixed_timestamp);
    }

    TEST_P(TimestampCounterTest, RelativeClockDoesNotUseNativeTimestamp)
    {
        execute(true);
    }

    INSTANTIATE_TEST_SUITE_P(RdtscAndRdtscp, TimestampCounterTest, testing::Bool());
}
