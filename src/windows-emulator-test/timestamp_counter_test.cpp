#include "std_include.hpp"

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
                this->cpu = this->backend.get();
                const auto instruction = GetParam() ? x86_hookable_instructions::rdtscp : x86_hookable_instructions::rdtsc;
                auto* hook = this->backend->hook_instruction(instruction, [] { return instruction_hook_continuation::run_instruction; });
                if (!hook)
                {
                    GTEST_SKIP() << "Backend does not support timestamp instruction hooks";
                }
                this->backend->delete_hook(hook);
            }

            windows_emulator create_emulator(const bool relative_time, std::unique_ptr<utils::clock> clock = {},
                                             emulator_callbacks observers = {})
            {
                emulator_interfaces interfaces{};
                interfaces.clock = std::move(clock);
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                return windows_emulator{std::move(this->backend),
                                        emulator_settings{.disable_logging = true,
                                                          .use_relative_time = relative_time,
                                                          .emulation_root = get_emulator_root(),
                                                          .load_registry = false},
                                        std::move(observers), std::move(interfaces)};
            }

            uint64_t run_timestamp_instruction(windows_emulator& emu)
            {
                emulator_thread thread{emu.memory, emu.process, 0, 0, 0x100000, 0, 8, true};
                emu.vcpu(0).active_thread = &thread;

                // RDTSC or RDTSCP, its result kept in r8 and r9, then CPUID, whose exit ends the run.
                const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
                const std::array<uint8_t, 3> rdtscp{0x0f, 0x01, 0xf9};
                const std::array<uint8_t, 2> rdtsc{0x0f, 0x31};
                const std::array<uint8_t, 8> keep_and_cpuid{0x49, 0x89, 0xc0, 0x49, 0x89, 0xd1, 0x0f, 0xa2};
                const auto instruction_size = GetParam() ? rdtscp.size() : rdtsc.size();
                emu.memory.write_memory(code, GetParam() ? rdtscp.data() : rdtsc.data(), instruction_size);
                emu.memory.write_memory(code + instruction_size, keep_and_cpuid.data(), keep_and_cpuid.size());
                auto* stop_at_cpuid = this->cpu->hook_instruction(x86_hookable_instructions::cpuid, [this](cpu_interface&, uint64_t) {
                    this->cpu->stop();
                    return instruction_hook_continuation::run_instruction;
                });

                this->cpu->reg(x86_register::rip, code);
                this->cpu->reg(x86_register::r8, UINT64_MAX);
                this->cpu->reg(x86_register::r9, UINT64_MAX);
                this->cpu->start();
                this->cpu->delete_hook(stop_at_cpuid);

                EXPECT_LE(this->cpu->reg(x86_register::r8), UINT32_MAX);
                EXPECT_LE(this->cpu->reg(x86_register::r9), UINT32_MAX);
                return this->cpu->reg(x86_register::r8) | (this->cpu->reg(x86_register::r9) << 32);
            }

            uint64_t execute(const bool relative_time, std::unique_ptr<utils::clock> clock = {})
            {
                size_t callbacks = 0;
                utils::clock* active_clock = nullptr;
                uint64_t expected_timestamp = 0;
                emulator_callbacks observers{};
                const auto observe = [&] {
                    ++callbacks;
                    if (relative_time)
                    {
                        expected_timestamp = active_clock->timestamp_counter();
                    }
                };
                observers.on_rdtsc = observe;
                observers.on_rdtscp = observe;
                auto emu = this->create_emulator(relative_time, std::move(clock), std::move(observers));
                active_clock = &emu.clock();
                const auto timestamp = this->run_timestamp_instruction(emu);
                EXPECT_EQ(callbacks, 1u);
                if (relative_time)
                {
                    EXPECT_EQ(timestamp, expected_timestamp);
                }
                return timestamp;
            }

            std::unique_ptr<x86_64_emulator> backend{};
            x86_64_emulator* cpu{};
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

    TEST_P(TimestampCounterTest, ObserverRegisteredLaterIsNotifiedAndKeepsNativeTimestamp)
    {
        auto emu = this->create_emulator(false);
        if (!emu.native_timestamp_counter_frequency() || !emu.intercepts_timestamp_counter())
        {
            GTEST_SKIP() << "The guest does not read its own TSC under the emulator's timing on this backend";
        }

        size_t observed = 0;
        emu.callbacks.on_rdtsc = [&] { ++observed; };
        emu.callbacks.on_rdtscp = [&] { ++observed; };
        emu.update_timestamp_counter_hooks();

        const auto before = emu.vcpu(0).cpu.read_native_timestamp_counter().value();
        const auto timestamp = this->run_timestamp_instruction(emu);
        const auto after = emu.vcpu(0).cpu.read_native_timestamp_counter().value();

        EXPECT_EQ(observed, 1u);
        EXPECT_GE(timestamp, before);
        EXPECT_LE(timestamp, after);
    }

    INSTANTIATE_TEST_SUITE_P(RdtscAndRdtscp, TimestampCounterTest, testing::Bool());
}
