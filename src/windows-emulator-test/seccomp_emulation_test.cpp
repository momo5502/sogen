#include "emulation_test_utils.hpp"

#include <cstring>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <system_error>

#ifdef SOGEN_ENABLE_SECCOMP
#include <asm/hwcap.h>
#include <platform/arm64_context.hpp>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <utils/finally.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <thread>
#include <unistd.h>

namespace sogen::test
{
    namespace
    {
        thread_local volatile sig_atomic_t host_signal_bits{};
        thread_local volatile sig_atomic_t host_signal_mask_invalid{};

        void host_signal_handler(const int signal, siginfo_t*, void*)
        {
            host_signal_bits = host_signal_bits | (1 << signal);
            sigset_t mask{};
            sigprocmask(SIG_SETMASK, nullptr, &mask);
            if (sigismember(&mask, SIGWINCH) != 1 || sigismember(&mask, signal) != 1)
            {
                host_signal_mask_invalid = 1;
            }
        }

        class host_signal_handlers
        {
          public:
            host_signal_handlers()
            {
                struct sigaction action{};
                action.sa_sigaction = host_signal_handler;
                action.sa_flags = SA_SIGINFO;
                sigemptyset(&action.sa_mask);
                sigaddset(&action.sa_mask, SIGWINCH);
                for (size_t i = 0; i < signals.size(); ++i)
                {
                    if (sigaction(signals[i], &action, &previous[i]) != 0)
                    {
                        throw std::runtime_error("Install host test signal handler");
                    }
                }
            }

            ~host_signal_handlers()
            {
                for (size_t i = 0; i < signals.size(); ++i)
                {
                    sigaction(signals[i], &previous[i], nullptr);
                }
            }

          private:
            static constexpr std::array signals{SIGUSR1, SIGUSR2, SIGILL};
            std::array<struct sigaction, signals.size()> previous{};
        };

        void check_host_signals()
        {
            uint64_t original_tls{};
            asm volatile("mrs %0, tpidr_el0" : "=r"(original_tls));
            host_signal_bits = 0;
            host_signal_mask_invalid = 0;
            for (const auto signal : {SIGUSR1, SIGUSR2, SIGILL})
            {
                errno = EDOM;
                EXPECT_EQ(raise(signal), 0);
                EXPECT_EQ(errno, EDOM);
                uint64_t tls{};
                asm volatile("mrs %0, tpidr_el0" : "=r"(tls));
                EXPECT_EQ(tls, original_tls);
            }
            EXPECT_EQ(host_signal_bits, (1 << SIGUSR1) | (1 << SIGUSR2) | (1 << SIGILL));
            EXPECT_EQ(host_signal_mask_invalid, 0);
        }

        class SeccompBackendTest : public testing::Test
        {
          protected:
            static constexpr uint64_t code_address = 0x10000;
            static constexpr uint64_t stack_address = 0x20000;
            std::unique_ptr<arm64_emulator> machine = create_arm64_emulator(backend_type::seccomp);
            memory_manager memory{*machine};

            void SetUp() override
            {
                ASSERT_TRUE(memory.allocate_memory(code_address, 0x1000, memory_permission::all));
                ASSERT_TRUE(memory.allocate_memory(stack_address, 0x1000, memory_permission::read_write));
                machine->reg(arm64_register::pc, code_address);
                machine->reg(arm64_register::sp, stack_address + 0xFF0);
            }
        };

        class SeccompCurrentElTest : public SeccompBackendTest, public testing::WithParamInterface<uint32_t>
        {
        };

        class SeccompBreakpointTest : public SeccompBackendTest, public testing::WithParamInterface<uint32_t>
        {
        };

        class SeccompLd1Test : public SeccompBackendTest, public testing::WithParamInterface<uint32_t>
        {
        };

        class SeccompByteLaneLoadTest : public SeccompBackendTest, public testing::WithParamInterface<uint32_t>
        {
        };

        class SeccompExecutionHookTest : public SeccompBackendTest
        {
          protected:
            size_t syscall_calls{};

            void SetUp() override
            {
                SeccompBackendTest::SetUp();
                machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
                    ++syscall_calls;
                    cpu.stop();
                    return instruction_hook_continuation::skip_instruction;
                });
            }
        };

        class SeccompWindowsSettingsTest : public testing::Test
        {
          protected:
            std::filesystem::path root =
                std::filesystem::temp_directory_path() /
                ("sogen-seccomp-settings-test-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));

            void SetUp() override
            {
                ASSERT_TRUE(std::filesystem::create_directory(this->root));
                ASSERT_TRUE(std::filesystem::create_directory(this->root / "filesys"));
            }

            void TearDown() override
            {
                std::error_code error{};
                std::filesystem::remove_all(this->root, error);
            }

            std::unique_ptr<windows_emulator> create(emulator_settings settings = {}, std::unique_ptr<utils::clock> clock = {},
                                                     emulator_callbacks callbacks = {})
            {
                settings.emulation_root = this->root;
                settings.load_registry = false;
                settings.disable_logging = true;
                emulator_interfaces interfaces{};
                interfaces.clock = std::move(clock);
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                return std::make_unique<windows_emulator>(create_arm64_emulator(backend_type::seccomp), settings, std::move(callbacks),
                                                          std::move(interfaces));
            }
        };
    }

    TEST(SeccompSignalTest, OtherHostThreadsKeepTheirTlsAndCannotEnterTheRunningGuest)
    {
        const host_signal_handlers handlers{};
        auto machine = create_arm64_emulator(backend_type::seccomp);
        memory_manager memory{*machine};
        ASSERT_TRUE(memory.allocate_memory(0x10000, 0x1000, memory_permission::all));
        ASSERT_TRUE(memory.allocate_memory(0x20000, 0x1000, memory_permission::read_write));
        machine->write_memory(0x10000, std::array{0x52800020u, 0x889FFC20u, 0x88DFFC40u, 0x34FFFFE0u, 0xD4000001u});
        machine->reg(arm64_register::pc, 0x10000);
        machine->reg(arm64_register::sp, 0x20FF0);
        machine->reg(arm64_register::x1, 0x20000);
        machine->reg(arm64_register::x2, 0x20004);
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        std::atomic_ref<uint32_t> entered{*std::construct_at(reinterpret_cast<uint32_t*>(0x20000), 0u)};
        std::atomic_ref<uint32_t> release{*std::construct_at(reinterpret_cast<uint32_t*>(0x20004), 0u)};
        std::jthread worker{[&](const std::stop_token stop) {
            while (entered.load(std::memory_order_acquire) == 0)
            {
                if (stop.stop_requested())
                {
                    return;
                }
                std::this_thread::yield();
            }
            check_host_signals();
            release.store(1, std::memory_order_release);
        }};

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(machine->read_instruction_pointer(), 0x10014u);
        EXPECT_FALSE(machine->has_violation());
    }

    TEST(SeccompSignalTest, OwnerHostCallbacksChainUnsolicitedEntryAndFaultSignals)
    {
        const host_signal_handlers handlers{};
        auto machine = create_arm64_emulator(backend_type::seccomp);
        memory_manager memory{*machine};
        ASSERT_TRUE(memory.allocate_memory(0x10000, 0x1000, memory_permission::all));
        ASSERT_TRUE(memory.allocate_memory(0x20000, 0x1000, memory_permission::read_write));
        machine->write_memory(0x10000, uint32_t{0xD4000001});
        machine->reg(arm64_register::pc, 0x10000);
        machine->reg(arm64_register::sp, 0x20FF0);
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            check_host_signals();
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(machine->read_instruction_pointer(), 0x10004u);
    }

    TEST_F(SeccompWindowsSettingsTest, DefaultTimingSettingsAllowConstructionWithNativeCounterReads)
    {
        const auto emu = create();
        const auto frequency = emu->native_timestamp_counter_frequency();
        ASSERT_TRUE(frequency.has_value());
        EXPECT_GT(*frequency, 0u);
    }

    TEST_F(SeccompWindowsSettingsTest, ExplicitCounterInterceptionIsRejected)
    {
        EXPECT_THROW(create({.intercept_rdtsc = true}), std::invalid_argument);
    }

    TEST_F(SeccompWindowsSettingsTest, RelativeTimeIsRejected)
    {
        EXPECT_THROW(create({.use_relative_time = true}), std::invalid_argument);
    }

    TEST_F(SeccompWindowsSettingsTest, InjectedClockIsRejected)
    {
        EXPECT_THROW(create({}, std::make_unique<utils::clock>()), std::invalid_argument);
    }

    TEST_F(SeccompWindowsSettingsTest, AccessViolationsBuildWindowsExceptionFrames)
    {
        arm64_cpu* cpu{};
        size_t faults{};
        emulator_callbacks callbacks{};
        callbacks.on_memory_violate = [&](uint64_t, size_t, memory_operation, memory_violation_type) {
            ++faults;
            cpu->stop();
        };
        const auto emu = create({}, {}, std::move(callbacks));
        cpu = dynamic_cast<arm64_cpu*>(&emu->emu());
        ASSERT_NE(cpu, nullptr);
        const auto code = emu->memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0u);
        emulator_thread thread{emu->memory, emu->process, code, 0, 0x10000, 0, 8, true};
        emu->vcpu(0).active_thread = &thread;
        const auto clear_thread = utils::finally([&] { emu->vcpu(0).active_thread = nullptr; });
        const auto initial_sp = thread.stack_base + thread.stack_size - 0x10;
        const auto dispatcher = code + 0x100;
        emu->process.ki_user_exception_dispatcher = dispatcher;
        constexpr std::array cases{
            std::pair{0xF9400020u, uint64_t{0}},
            std::pair{0xF9000020u, uint64_t{1}},
            std::pair{0xD61F0020u, uint64_t{8}},
        };
        for (const auto [instruction, access_type] : cases)
        {
            emu->memory.write_memory(code, instruction);
            cpu->reg(arm64_register::pc, code);
            cpu->reg(arm64_register::sp, initial_sp);
            cpu->reg(arm64_register::x0, 0x1234u);
            cpu->reg(arm64_register::x1, 0);
            cpu->reg(arm64_register::pstate, 0xA0000000u);

            ASSERT_NO_THROW(cpu->start());
            EXPECT_EQ(cpu->read_instruction_pointer(), dispatcher);
            const auto sp = cpu->read_stack_pointer();
            const auto context = emu->memory.read_memory<arm64_context>(sp);
            const auto record = emu->memory.read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(sp + 0x3B0);
            const auto faulting_pc = access_type == 8 ? 0 : code;
            EXPECT_EQ(record.ExceptionCode, static_cast<DWORD>(STATUS_ACCESS_VIOLATION));
            EXPECT_EQ(record.ExceptionAddress, faulting_pc);
            ASSERT_EQ(record.NumberParameters, 2u);
            EXPECT_EQ(record.ExceptionInformation[0], access_type);
            EXPECT_EQ(record.ExceptionInformation[1], 0u);
            EXPECT_EQ(context.Pc, faulting_pc);
            EXPECT_EQ(context.Sp, initial_sp);
            EXPECT_EQ(context.X[0], 0x1234u);
            EXPECT_EQ(context.X[1], 0u);
            EXPECT_EQ(context.Cpsr, 0xA0000000u);
        }
        EXPECT_EQ(faults, cases.size());
    }

    TEST_P(SeccompCurrentElTest, RaisesUndefinedInstructionWithoutChangingGuestState)
    {
        constexpr uint32_t mrs_current_el = 0xD5384240;
        constexpr uint32_t svc = 0xD4000001 | (0x1234 << 5);
        const auto destination = GetParam();
        machine->write_memory(code_address, std::array{mrs_current_el | destination, svc});
        const auto register_id = [](const uint32_t index) {
            if (index < 29)
            {
                return static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::x0) + index);
            }
            return index == 29 ? arm64_register::x29 : arm64_register::x30;
        };
        for (uint32_t i = 0; i < 31; ++i)
        {
            machine->reg(register_id(i), 0xABC000ull + i);
        }
        machine->reg(arm64_register::nzcv, 0xA0000000u);
        size_t exceptions{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++exceptions;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::undefined_instruction));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->read_stack_pointer(), stack_address + 0xFF0);
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            for (uint32_t i = 0; i < 31; ++i)
            {
                EXPECT_EQ(machine->reg(register_id(i)), 0xABC000ull + i) << "x" << i;
            }
            machine->reg(arm64_register::pc, code_address + 4);
        });
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, const uint64_t service) {
            ++calls;
            EXPECT_EQ(service, 0x1234u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->read_stack_pointer(), stack_address + 0xFF0);
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            for (uint32_t i = 0; i < 31; ++i)
            {
                EXPECT_EQ(machine->reg(register_id(i)), 0xABC000ull + i) << "x" << i;
            }
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(exceptions, 1u);
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        EXPECT_FALSE(machine->has_violation());
    }

    INSTANTIATE_TEST_SUITE_P(Destination, SeccompCurrentElTest, testing::Values(0u, 16u, 30u, 31u));

    TEST_P(SeccompBreakpointTest, ReportsTheFaultingPcAndCancelsPendingSoftwareStep)
    {
        const uint32_t instruction = 0xD4200000 | (GetParam() << 5);
        machine->write_memory(code_address, std::array{instruction, 0xD4000001u});
        machine->reg(arm64_register::x0, 0x1234u);
        machine->reg(arm64_register::x16, 0x5678u);
        size_t exceptions{};
        machine->hook_interrupt([&](cpu_interface& cpu, const int exception_class) {
            ++exceptions;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::breakpoint_instruction));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->read_stack_pointer(), stack_address + 0xFF0);
            EXPECT_EQ(machine->reg(arm64_register::x0), 0x1234u);
            EXPECT_EQ(machine->reg(arm64_register::x16), 0x5678u);
            EXPECT_EQ(machine->reg(arm64_register::pstate), 0xA0000000u);
            cpu.stop();
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            ADD_FAILURE() << "Execution must stop at BRK";
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto software_step : {0u, 1u << 21})
        {
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::pstate, 0xA0000000u | software_step);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_FALSE(machine->has_violation());
        }
        EXPECT_EQ(exceptions, 2u);
    }

    INSTANTIATE_TEST_SUITE_P(Immediate, SeccompBreakpointTest, testing::Values(0xF000u, 0xF001u, 0xF003u));

    TEST_F(SeccompBackendTest, IllegalInstructionsWithoutAnInterruptHookFail)
    {
        machine->write_memory(code_address, std::array{0x00000000u, 0xD4000001u});
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            ++calls;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);
        EXPECT_TRUE(machine->has_violation());
    }

    TEST_F(SeccompBackendTest, SoftwareStepExecutesOneInstructionAndCanBeRearmed)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, std::array{0xD2800020u, 0x91000400u, 0x91000400u, 0xD4000001u});
        machine->reg(arm64_register::pstate, software_step);
        machine->reg(arm64_register::nzcv, 0x60000000u);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4 * steps);
            EXPECT_EQ(machine->reg(arm64_register::x0), steps);
            EXPECT_EQ(machine->reg(arm64_register::pstate), 0x60000000u);
            if (steps == 1)
            {
                machine->reg(arm64_register::pstate, 0x60000000u | software_step);
            }
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(steps, 2u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 16);
    }

    TEST_F(SeccompBackendTest, SoftwareStepUsesTheExecutedBranchTargetAndLinkRegister)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, uint32_t{0x14000000});
        machine->reg(arm64_register::pstate, software_step);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface& cpu, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->reg(arm64_register::pstate) & software_step, 0u);
            cpu.stop();
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface&, uint64_t) {
            ADD_FAILURE() << "The step must stop before executing the branch target";
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);

        machine->write_memory(code_address, std::array{0x94000002u, 0xD503201Fu, 0xD4000001u});
        machine->reg(arm64_register::pc, code_address);
        machine->reg(arm64_register::pstate, software_step);
        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(steps, 2u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        EXPECT_EQ(machine->reg(arm64_register::x30), code_address + 4);
    }

    TEST_F(SeccompBackendTest, FaultingInstructionCancelsThePendingSoftwareStep)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, std::array{0xD5384250u, 0xD4000001u});
        machine->reg(arm64_register::x16, 0x1234u);
        machine->reg(arm64_register::pstate, software_step);
        size_t exceptions{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++exceptions;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::undefined_instruction));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->reg(arm64_register::pstate) & software_step, 0u);
            EXPECT_EQ(machine->reg(arm64_register::x16), 0x1234u);
            machine->reg(arm64_register::pc, code_address + 4);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(exceptions, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
    }

    TEST_F(SeccompBackendTest, SteppedSvcRaisesOnlyTheSyscall)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, uint32_t{0xD4000001 | (0x1234 << 5)});
        machine->reg(arm64_register::pstate, software_step);
        size_t executions{};
        machine->hook_memory_execution(code_address, [&](cpu_interface&, uint64_t) {
            ++executions;
            EXPECT_EQ(machine->reg(arm64_register::pstate) & software_step, software_step);
        });
        machine->hook_interrupt([](cpu_interface&, int) { ADD_FAILURE() << "SVC must cancel the software-step exception"; });
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, const uint64_t service) {
            ++calls;
            EXPECT_EQ(service, 0x1234u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->reg(arm64_register::pstate) & software_step, 0u);
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(executions, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
    }

    TEST_F(SeccompExecutionHookTest, OneShotRangeTrapsABranchBeforeItsFirstInstruction)
    {
        constexpr uint64_t target = 0x40000;
        ASSERT_TRUE(memory.allocate_memory(target, 0x1000, memory_permission::read_exec));
        machine->write_memory(code_address, std::array{0x1400C000u, 0xD4000001u});
        machine->reg(arm64_register::x0, 0x1234u);
        machine->reg(arm64_register::x2, code_address + 4);
        machine->reg(arm64_register::pstate, 0xA0000000u);
        size_t executions{};
        emulator_hook* hook{};
        hook = machine->hook_memory_range_execution(target, 0x1000, [&](cpu_interface&, const uint64_t address) {
            ++executions;
            EXPECT_EQ(address, target);
            EXPECT_EQ(machine->read_instruction_pointer(), target);
            EXPECT_EQ(machine->reg(arm64_register::x0), 0x1234u);
            EXPECT_EQ(machine->reg(arm64_register::pstate), 0xA0000000u);
            machine->delete_hook(hook);
        });
        machine->write_memory(target, std::array{0xD28000E0u, 0xD61F0040u});

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 1u);
        EXPECT_EQ(syscall_calls, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 7u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        EXPECT_FALSE(machine->has_violation());
    }

    TEST_F(SeccompExecutionHookTest, PersistentRangesRearmAndExcludeOtherInstructionsOnTheirPage)
    {
        machine->write_memory(code_address, std::array{0x91000400u, 0x91000400u, 0x91000400u, 0xD4000001u});
        std::vector<uint64_t> executions{};
        machine->hook_memory_range_execution(code_address + 4, 8, [&](cpu_interface&, const uint64_t address) {
            executions.push_back(address);
            EXPECT_EQ(machine->reg(arm64_register::x0), (address - code_address) / 4);
        });
        machine->hook_interrupt([](cpu_interface&, int) { ADD_FAILURE() << "Internal steps must not become guest exceptions"; });
        for (size_t run = 0; run < 2; ++run)
        {
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x0, 0);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 16);
        }
        EXPECT_EQ(executions, (std::vector<uint64_t>{code_address + 4, code_address + 8, code_address + 4, code_address + 8}));
        EXPECT_EQ(syscall_calls, 2u);
    }

    TEST_F(SeccompExecutionHookTest, GuestBranchStepPrecedesFirstExecutionAtTheTarget)
    {
        constexpr uint64_t target = 0x40000;
        ASSERT_TRUE(memory.allocate_memory(target, 0x1000, memory_permission::read_exec));
        machine->write_memory(code_address, std::array{0x1400C000u, 0xD4000001u});
        machine->write_memory(target, std::array{0xD28000E0u, 0xD61F0040u});
        machine->reg(arm64_register::x0, 0x1234u);
        machine->reg(arm64_register::x2, code_address + 4);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t executions{};
        size_t steps{};
        emulator_hook* hook{};
        hook = machine->hook_memory_execution(target, [&](cpu_interface&, uint64_t) {
            ++executions;
            EXPECT_EQ(steps, 1u);
            EXPECT_EQ(machine->reg(arm64_register::x0), 0x1234u);
            machine->delete_hook(hook);
        });
        machine->hook_interrupt([&](cpu_interface& cpu, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), target);
            EXPECT_EQ(executions, 0u);
            cpu.stop();
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 0u);
        EXPECT_EQ(machine->read_instruction_pointer(), target);
        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 1u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 7u);
        EXPECT_EQ(syscall_calls, 1u);
    }

    TEST_F(SeccompExecutionHookTest, OverlappingHooksCanDeleteThemselvesWithoutDisablingTheirNeighbor)
    {
        machine->write_memory(code_address, std::array{0x91000400u, 0x91000400u, 0x91000400u, 0xD4000001u});
        machine->reg(arm64_register::x0, 0);
        std::vector<uint64_t> executions{};
        emulator_hook* range{};
        emulator_hook* point{};
        range = machine->hook_memory_range_execution(code_address, 12, [&](cpu_interface&, const uint64_t address) {
            executions.push_back(address);
            EXPECT_EQ(machine->reg(arm64_register::x0), 0u);
            machine->delete_hook(range);
        });
        point = machine->hook_memory_execution(code_address + 4, [&](cpu_interface&, const uint64_t address) {
            executions.push_back(address);
            EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
            machine->delete_hook(point);
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, (std::vector<uint64_t>{code_address, code_address + 4}));
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
        EXPECT_EQ(syscall_calls, 1u);
    }

    TEST_F(SeccompExecutionHookTest, StoppedHooksResumeOnceAndPreserveTheGuestSoftwareStep)
    {
        machine->write_memory(code_address, std::array{0x91000400u, 0x91000400u, 0xD4000001u});
        size_t executions{};
        machine->hook_memory_execution(code_address, [&](cpu_interface& cpu, uint64_t) {
            ++executions;
            cpu.stop();
        });
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
        });
        for (const auto software_step : {0u, 1u << 21})
        {
            const auto previous_executions = executions;
            steps = 0;
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x0, 0);
            machine->reg(arm64_register::pstate, 0xA0000000u | software_step);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(executions, previous_executions + 1);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->reg(arm64_register::x0), 0u);
            EXPECT_EQ(machine->reg(arm64_register::pstate), 0xA0000000u | software_step);

            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(executions, previous_executions + 1);
            EXPECT_EQ(machine->reg(arm64_register::x0), 2u);
            EXPECT_EQ(machine->reg(arm64_register::pstate), 0xA0000000u);
            EXPECT_EQ(steps, software_step != 0 ? 1u : 0u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
        }
        EXPECT_EQ(syscall_calls, 2u);
    }

    TEST_F(SeccompExecutionHookTest, CallbacksCanRedirectExecutionWithoutLosingTheGuestSoftwareStep)
    {
        machine->write_memory(code_address, std::array{0xD4200000u, 0x91000400u, 0xD4000001u});
        machine->reg(arm64_register::x0, 0);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t executions{};
        machine->hook_memory_execution(code_address, [&](cpu_interface&, uint64_t) {
            ++executions;
            machine->reg(arm64_register::pc, code_address + 4);
        });
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
            EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 1u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(syscall_calls, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
    }

    TEST_F(SeccompExecutionHookTest, HooksSurviveMappingProtectionChangesAndRemapping)
    {
        constexpr uint64_t target = 0x40000;
        machine->write_memory(code_address, std::array{0x1400C000u, 0xD4000001u});
        machine->reg(arm64_register::x2, code_address + 4);
        size_t executions{};
        auto* hook = machine->hook_memory_execution(target, [&](cpu_interface&, const uint64_t address) {
            ++executions;
            EXPECT_EQ(address, target);
        });
        ASSERT_TRUE(memory.allocate_memory(target, 0x1000, memory_permission::read_write));
        machine->write_memory(target, std::array{0xD28000A0u, 0xD61F0040u});
        ASSERT_TRUE(memory.protect_memory(target, 0x1000, memory_permission::read_exec));
        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 5u);

        ASSERT_TRUE(memory.release_memory(target, 0x1000));
        ASSERT_TRUE(memory.allocate_memory(target, 0x1000, memory_permission::read_exec));
        machine->write_memory(target, std::array{0xD2800120u, 0xD61F0040u});
        machine->reg(arm64_register::pc, code_address);
        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 2u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 9u);

        machine->delete_hook(hook);
        machine->reg(arm64_register::pc, code_address);
        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 2u);
        EXPECT_EQ(syscall_calls, 3u);
    }

    TEST_F(SeccompExecutionHookTest, GuestExceptionsCancelInternalAndGuestSteps)
    {
        size_t executions{};
        machine->hook_memory_execution(code_address, [&](cpu_interface&, uint64_t) { ++executions; });
        size_t exceptions{};
        int expected_exception{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++exceptions;
            EXPECT_EQ(exception_class, expected_exception);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->reg(arm64_register::pstate) & (1u << 21), 0u);
            machine->reg(arm64_register::pc, code_address + 4);
        });
        for (const auto instruction : {0xD5384250u, 0xD43E0000u})
        {
            expected_exception = static_cast<int>(instruction == 0xD5384250 ? arm64_exception_class::undefined_instruction
                                                                            : arm64_exception_class::breakpoint_instruction);
            machine->write_memory(code_address, std::array{instruction, 0xD4000001u});
            for (const auto software_step : {0u, 1u << 21})
            {
                machine->reg(arm64_register::pc, code_address);
                machine->reg(arm64_register::pstate, software_step);
                ASSERT_NO_THROW(machine->start());
            }
        }
        EXPECT_EQ(executions, 4u);
        EXPECT_EQ(exceptions, 4u);
        EXPECT_EQ(syscall_calls, 4u);
    }

    TEST_F(SeccompExecutionHookTest, FailedHookDeletionCanBeRetriedWithoutDisablingLaterHooks)
    {
        constexpr uint64_t target = 0x40000;
        constexpr size_t size = 0x1000;
        auto* const target_pointer = reinterpret_cast<void*>(target);
        ASSERT_TRUE(memory.allocate_memory(target, size, memory_permission::all));
        auto* hook = machine->hook_memory_execution(target, [](cpu_interface&, uint64_t) {});
        size_t executions{};
        emulator_hook* remaining{};
        remaining = machine->hook_memory_execution(target, [&](cpu_interface&, uint64_t) {
            ++executions;
            machine->delete_hook(remaining);
        });
        ASSERT_EQ(munmap(target_pointer, size), 0);
        EXPECT_THROW(machine->delete_hook(hook), std::system_error);

        auto* const restored = mmap(target_pointer, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ASSERT_NE(restored, MAP_FAILED);
        const auto cleanup = utils::finally([&] {
            if (restored != target_pointer)
            {
                munmap(restored, size);
            }
        });
        ASSERT_EQ(restored, target_pointer);
        ASSERT_NO_THROW(machine->delete_hook(hook));

        machine->write_memory(code_address, std::array{0x1400C000u, 0xD4000001u});
        machine->write_memory(target, std::array{0xD28000E0u, 0xD61F0040u});
        machine->reg(arm64_register::x2, code_address + 4);

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(executions, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 7u);
        EXPECT_EQ(syscall_calls, 1u);
    }

    TEST_F(SeccompExecutionHookTest, MmioCompletionReportsOnlyGuestRequestedSteps)
    {
        constexpr uint64_t mmio_address = 0x40000;
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](uint64_t, void* output, const size_t size) {
                                             ++reads;
                                             const uint64_t value = 7;
                                             ASSERT_EQ(size, sizeof(value));
                                             std::memcpy(output, &value, size);
                                         },
                                         {}));
        machine->write_memory(code_address, std::array{0xF9400020u, 0xD4000001u});
        machine->reg(arm64_register::x1, mmio_address);
        size_t executions{};
        machine->hook_memory_range_execution(code_address, 8, [&](cpu_interface&, uint64_t) { ++executions; });
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x0), 7u);
        });
        for (const auto software_step : {0u, 1u << 21})
        {
            steps = 0;
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::pstate, software_step);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(steps, software_step != 0 ? 1u : 0u);
            EXPECT_EQ(machine->reg(arm64_register::x0), 7u);
        }
        EXPECT_EQ(executions, 4u);
        EXPECT_EQ(reads, 2u);
        EXPECT_EQ(syscall_calls, 2u);
    }

    TEST_F(SeccompBackendTest, SteppedSvcRequiresAnExecutablePage)
    {
        machine->write_memory(code_address, uint32_t{0xD4000001});
        machine->hook_memory_execution(
            code_address, [](cpu_interface&, uint64_t) { ADD_FAILURE() << "Execution hooks must not bypass guest execution protection"; });
        machine->hook_interrupt([](cpu_interface&, int) { ADD_FAILURE() << "An execution fault must cancel the software step"; });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            ADD_FAILURE() << "A non-executable SVC must not dispatch a syscall";
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        size_t faults{};
        machine->hook_memory_violation([&](cpu_interface&, const uint64_t address, const size_t size, const memory_operation operation,
                                           const memory_violation_type type) {
            ++faults;
            EXPECT_EQ(address, code_address);
            EXPECT_EQ(size, 4u);
            EXPECT_EQ(operation, memory_operation::exec);
            EXPECT_EQ(type, memory_violation_type::protection);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->reg(arm64_register::pstate), 0xA0000000u);
            return memory_violation_continuation::stop;
        });
        for (const auto permission : {memory_permission::none, memory_permission::read, memory_permission::read_write,
                                      memory_permission::all | memory_permission::guard})
        {
            ASSERT_TRUE(memory.protect_memory(code_address, 0x1000, permission));
            machine->reg(arm64_register::pstate, 0xA0000000u | (1u << 21));
            ASSERT_NO_THROW(machine->start());
            EXPECT_FALSE(machine->has_violation());
        }
        EXPECT_EQ(faults, 4u);
    }

    TEST_F(SeccompBackendTest, MemoryFaultsReportGuestAccessAndPreserveRegisters)
    {
        constexpr uint64_t data_address = 0x40000;
        ASSERT_TRUE(memory.allocate_memory(data_address, 0x1000, memory_permission::read));

        struct fault_case
        {
            uint32_t instruction;
            uint64_t address;
            memory_operation operation;
            memory_violation_type type;
            memory_permission permission = memory_permission::read;
        };

        constexpr std::array cases{
            fault_case{0xF9400020, 0, memory_operation::read, memory_violation_type::unmapped},
            fault_case{0xF9400020, 0x50000, memory_operation::read, memory_violation_type::unmapped},
            fault_case{0xF9400020, data_address, memory_operation::read, memory_violation_type::protection, memory_permission::none},
            fault_case{0xF8008420, 0, memory_operation::write, memory_violation_type::unmapped},
            fault_case{0xF8008420, data_address, memory_operation::write, memory_violation_type::protection},
        };
        machine->hook_interrupt([](cpu_interface&, int) { ADD_FAILURE() << "A memory fault must cancel the software step"; });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            ADD_FAILURE() << "The faulting instruction must not complete";
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto& fault : cases)
        {
            ASSERT_TRUE(memory.protect_memory(data_address, 0x1000, fault.permission));
            size_t faults{};
            auto* hook = machine->hook_memory_violation([&](cpu_interface&, const uint64_t address, const size_t size,
                                                            const memory_operation operation, const memory_violation_type type) {
                ++faults;
                EXPECT_EQ(address, fault.address);
                EXPECT_EQ(size, 8u);
                EXPECT_EQ(operation, fault.operation);
                EXPECT_EQ(type, fault.type);
                EXPECT_EQ(machine->read_instruction_pointer(), code_address);
                EXPECT_EQ(machine->reg(arm64_register::x0), 0x1234u);
                EXPECT_EQ(machine->reg(arm64_register::x1), fault.address);
                EXPECT_EQ(machine->reg(arm64_register::pstate), 0xA0000000u);
                return memory_violation_continuation::stop;
            });
            for (const auto software_step : {0u, 1u << 21})
            {
                machine->write_memory(code_address, std::array{fault.instruction, 0xD4000001u});
                machine->reg(arm64_register::pc, code_address);
                machine->reg(arm64_register::x0, 0x1234u);
                machine->reg(arm64_register::x1, fault.address);
                machine->reg(arm64_register::pstate, 0xA0000000u | software_step);
                ASSERT_NO_THROW(machine->start());
                EXPECT_FALSE(machine->has_violation());
            }
            EXPECT_EQ(faults, 2u);
            machine->delete_hook(hook);
        }
    }

    TEST_F(SeccompBackendTest, InstructionFetchFaultsIncludeBranchTargetsOutsideTheGuestWindow)
    {
        machine->write_memory(code_address, uint32_t{0xD61F0020});
        machine->reg(arm64_register::x1, 0);
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface&, uint64_t) {
            ADD_FAILURE() << "An invalid branch target must fault";
            return instruction_hook_continuation::skip_instruction;
        });
        size_t faults{};
        machine->hook_memory_violation([&](cpu_interface&, const uint64_t address, const size_t size, const memory_operation operation,
                                           const memory_violation_type type) {
            ++faults;
            EXPECT_EQ(address, 0u);
            EXPECT_EQ(size, 4u);
            EXPECT_EQ(operation, memory_operation::exec);
            EXPECT_EQ(type, memory_violation_type::unmapped);
            EXPECT_EQ(machine->read_instruction_pointer(), 0u);
            return memory_violation_continuation::stop;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(faults, 1u);
        EXPECT_FALSE(machine->has_violation());
    }

    TEST_F(SeccompBackendTest, MemoryFaultCallbacksCanRedirectExecution)
    {
        machine->write_memory(code_address, std::array{0xF9400020u, 0xD4000001u});
        machine->reg(arm64_register::x1, 0);
        machine->hook_memory_violation([&](cpu_interface&, uint64_t, size_t, memory_operation, memory_violation_type) {
            machine->reg(arm64_register::pc, code_address + 4);
            return memory_violation_continuation::resume;
        });
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            ++calls;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(calls, 1u);
        EXPECT_FALSE(machine->has_violation());
    }

    TEST_F(SeccompBackendTest, MemoryFaultRestartRequestsFailExplicitly)
    {
        machine->write_memory(code_address, uint32_t{0xF9400020});
        machine->reg(arm64_register::x1, 0);
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface&, uint64_t) {
            ADD_FAILURE() << "The faulting load must not complete";
            return instruction_hook_continuation::skip_instruction;
        });
        machine->hook_memory_violation([](cpu_interface&, uint64_t, size_t, memory_operation, memory_violation_type) {
            return memory_violation_continuation::restart;
        });

        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);
    }

    TEST_F(SeccompBackendTest, MmioAcquireLoadsMatchNativeAlignmentChecks)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr uint64_t ram_address = 0x50000;
        std::array<uint8_t, 32> data{};
        for (size_t i = 0; i < data.size(); ++i)
        {
            data[i] = static_cast<uint8_t>(i);
        }
        ASSERT_TRUE(memory.allocate_memory(ram_address, 0x1000, memory_permission::read));
        machine->write_memory(ram_address, data.data(), data.size());
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             ++reads;
                                             ASSERT_LE(offset + size, data.size());
                                             std::memcpy(output, data.data() + offset, size);
                                         },
                                         {}));
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
        });
        machine->hook_memory_violation([](cpu_interface&, uint64_t, size_t, memory_operation, memory_violation_type) {
            ADD_FAILURE() << "An alignment fault is not an access violation";
            return memory_violation_continuation::stop;
        });
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            ++calls;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        struct load_case
        {
            uint32_t instruction;
            size_t size;
            unsigned long capability{};
        };

        constexpr std::array cases{
            load_case{0x08DFFC20, 1},
            load_case{0x48DFFC20, 2},
            load_case{0x88DFFC20, 4},
            load_case{0xC8DFFC20, 8},
            load_case{0x38BFC020, 1, HWCAP_LRCPC},
            load_case{0x78BFC020, 2, HWCAP_LRCPC},
            load_case{0xB8BFC020, 4, HWCAP_LRCPC},
            load_case{0xF8BFC020, 8, HWCAP_LRCPC},
            load_case{0x19400020, 1, HWCAP_ILRCPC},
            load_case{0x59400020, 2, HWCAP_ILRCPC},
            load_case{0x99400020, 4, HWCAP_ILRCPC},
            load_case{0xD9400020, 8, HWCAP_ILRCPC},
        };
        size_t native_alignment_faults{};
        for (const auto& load : cases)
        {
            if (load.capability != 0 && (getauxval(AT_HWCAP) & load.capability) == 0)
            {
                continue;
            }
            machine->write_memory(code_address, std::array{load.instruction, 0xD4000001u});
            for (const auto offset : {0u, 1u, 9u, 15u})
            {
                SCOPED_TRACE(testing::Message() << "instruction=" << load.instruction << ", offset=" << offset);
                uint64_t expected{};
                std::memcpy(&expected, data.data() + offset, load.size);
                for (const auto software_step : {0u, 1u << 21})
                {
                    const auto run = [&](const uint64_t base) {
                        calls = steps = reads = 0;
                        machine->reg(arm64_register::pc, code_address);
                        machine->reg(arm64_register::x0, 0x1234u);
                        machine->reg(arm64_register::x1, base + offset);
                        machine->reg(arm64_register::pstate, 0xA0000000u | software_step);
                        bool completed{};
                        try
                        {
                            machine->start();
                            completed = true;
                        }
                        catch (const std::runtime_error&)
                        {
                            EXPECT_TRUE(machine->has_violation());
                        }
                        EXPECT_EQ(machine->reg(arm64_register::x1), base + offset);
                        EXPECT_EQ(machine->reg(arm64_register::pstate), 0xA0000000u);
                        EXPECT_EQ(machine->reg(arm64_register::x0), completed ? expected : 0x1234u);
                        EXPECT_EQ(machine->read_instruction_pointer(), completed ? code_address + 8 : code_address);
                        EXPECT_EQ(calls, completed ? 1u : 0u);
                        EXPECT_EQ(steps, completed && software_step != 0 ? 1u : 0u);
                        EXPECT_EQ(reads, completed && base == mmio_address ? 1u : 0u);
                        return completed;
                    };
                    const auto native_completed = run(ram_address);
                    native_alignment_faults += !native_completed;
                    const auto mmio_completed = run(mmio_address);
                    EXPECT_EQ(mmio_completed, native_completed);
                }
            }
        }
        RecordProperty("native_alignment_faults", std::to_string(native_alignment_faults));
    }

    TEST_F(SeccompBackendTest, MmioLoadsPreserveArm64RegisterSemantics)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 16> data{0x80, 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 8, 9, 10, 11, 12, 13, 14, 15};

        struct load_case
        {
            uint32_t instruction;
            size_t size;
            uint64_t result;
            uint64_t base_offset = 0x38;
        };

        constexpr std::array cases{
            load_case{0x39400020, 1, 0x80},
            load_case{0x39800020, 1, 0xFFFFFFFFFFFFFF80},
            load_case{0x39C00020, 1, 0xFFFFFF80},
            load_case{0x79400020, 2, 0xFF80},
            load_case{0x79800020, 2, 0xFFFFFFFFFFFFFF80},
            load_case{0x79C00020, 2, 0xFFFFFF80},
            load_case{0xB9400020, 4, 0xFDFEFF80},
            load_case{0xB9800020, 4, 0xFFFFFFFFFDFEFF80},
            load_case{0xF9400020, 8, 0xF9FAFBFCFDFEFF80},
            load_case{0xF9400420, 8, 0xF9FAFBFCFDFEFF80, 0x30},
            load_case{0xF85F8020, 8, 0xF9FAFBFCFDFEFF80, 0x40},
            load_case{0xF8626820, 8, 0xF9FAFBFCFDFEFF80},
            load_case{0x88DFFC20, 4, 0xFDFEFF80},
        };
        size_t expected_size{};
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(
            mmio_address, 0x1000,
            [&](const uint64_t offset, void* output, const size_t size) {
                ++reads;
                EXPECT_EQ(offset, 0x38u);
                EXPECT_EQ(size, expected_size);
                ASSERT_LE(size, data.size());
                std::memcpy(output, data.data(), size);
            },
            [](uint64_t, const void*, size_t) { ADD_FAILURE() << "MMIO writes are unsupported"; }));
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto& load : cases)
        {
            SCOPED_TRACE(load.instruction);
            expected_size = load.size;
            machine->write_memory(code_address, std::array{load.instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x0, ~uint64_t{});
            machine->reg(arm64_register::x1, mmio_address + load.base_offset);
            machine->reg(arm64_register::x2, 0u);
            machine->reg(arm64_register::nzcv, 0xA0000000u);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(machine->reg(arm64_register::x0), load.result);
            EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + load.base_offset);
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
            EXPECT_FALSE(machine->has_violation());
        }
        EXPECT_EQ(reads, cases.size());

        for (const auto instruction : {0x3D400020u, 0x3DC00020u, 0x3CC00020u})
        {
            expected_size = instruction == 0x3D400020u ? 1 : 16;
            std::array<uint8_t, 16> initial{};
            initial.fill(0xFF);
            machine->write_memory(code_address, std::array{instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x1, mmio_address + 0x38);
            machine->write_register(arm64_register::q0, initial.data(), initial.size());
            ASSERT_NO_THROW(machine->start());
            std::array<uint8_t, 16> result{};
            machine->read_register(arm64_register::q0, result.data(), result.size());
            auto expected = data;
            std::fill(expected.begin() + expected_size, expected.end(), 0);
            EXPECT_EQ(result, expected);
        }
        EXPECT_EQ(reads, cases.size() + 3);
    }

    TEST_F(SeccompBackendTest, IndexedMmioLoadsWriteBackTheBaseAfterReading)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 16> data{0x80, 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 8, 9, 10, 11, 12, 13, 14, 15};

        struct load_case
        {
            uint32_t instruction;
            arm64_register base;
            size_t size;
            uint64_t read_offset;
            uint64_t updated_offset;
            uint64_t result;
            bool vector = false;
        };

        constexpr std::array cases{
            load_case{0x78400402, arm64_register::x0, 2, 0x400, 0x400, 0xFF80},
            load_case{0x78408C02, arm64_register::x0, 2, 0x408, 0x408, 0xFF80},
            load_case{0x785F8C02, arm64_register::x0, 2, 0x3F8, 0x3F8, 0xFF80},
            load_case{0x784FF402, arm64_register::x0, 2, 0x400, 0x4FF, 0xFF80},
            load_case{0x78500C02, arm64_register::x0, 2, 0x300, 0x300, 0xFF80},
            load_case{0x785F8402, arm64_register::x0, 2, 0x400, 0x3F8, 0xFF80},
            load_case{0xB8808402, arm64_register::x0, 4, 0x400, 0x408, 0xFFFFFFFFFDFEFF80},
            load_case{0x3CC10C00, arm64_register::x0, 16, 0x410, 0x410, 0, true},
            load_case{0x3CDF0400, arm64_register::x0, 16, 0x400, 0x3F0, 0, true},
            load_case{0xF84107E2, arm64_register::sp, 8, 0x400, 0x410, 0xF9FAFBFCFDFEFF80},
            load_case{0xF85F0FE2, arm64_register::sp, 8, 0x3F0, 0x3F0, 0xF9FAFBFCFDFEFF80},
            load_case{0xF840841F, arm64_register::x0, 8, 0x400, 0x408, 0x1234},
        };
        const load_case* current{};
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             ++reads;
                                             EXPECT_EQ(offset, current->read_offset);
                                             ASSERT_EQ(size, current->size);
                                             EXPECT_EQ(machine->reg(current->base), mmio_address + 0x400);
                                             std::memcpy(output, data.data(), size);
                                         },
                                         {}));
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto& load : cases)
        {
            SCOPED_TRACE(load.instruction);
            current = &load;
            machine->write_memory(code_address, std::array{load.instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::sp, stack_address + 0xFF0);
            machine->reg(load.base, mmio_address + 0x400);
            machine->reg(arm64_register::x2, 0x1234u);
            machine->reg(arm64_register::nzcv, 0xA0000000u);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(machine->reg(load.base), mmio_address + load.updated_offset);
            if (load.vector)
            {
                std::array<uint8_t, 16> result{};
                machine->read_register(arm64_register::q0, result.data(), result.size());
                EXPECT_EQ(result, data);
            }
            else
            {
                EXPECT_EQ(machine->reg(arm64_register::x2), load.result);
            }
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        }
        EXPECT_EQ(reads, cases.size());

        current = &cases[1];
        ASSERT_TRUE(memory.release_memory(mmio_address, 0x1000));
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [](uint64_t, void*, size_t) { throw std::runtime_error("MMIO device read failed"); }, {}));
        machine->write_memory(code_address, std::array{current->instruction, 0xD4000001u});
        machine->reg(arm64_register::pc, code_address);
        machine->reg(current->base, mmio_address + 0x400);
        machine->reg(arm64_register::x2, 0x1234u);
        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_EQ(machine->reg(current->base), mmio_address + 0x400);
        EXPECT_EQ(machine->reg(arm64_register::x2), 0x1234u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);
    }

    TEST_P(SeccompLd1Test, LoadsOneVectorAndWritesBackAfterEachRead)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 32> data{0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                               16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             ASSERT_LT(reads, 2u);
                                             ASSERT_EQ(size, 16u);
                                             EXPECT_EQ(offset, 0x40 + 16 * reads);
                                             EXPECT_EQ(machine->reg(arm64_register::x5), mmio_address + 0x40 + 16 * reads);
                                             std::memcpy(output, data.data() + 16 * reads, size);
                                             ++reads;
                                         },
                                         {}));
        const uint32_t instruction = 0x4CDF70A0u | (GetParam() << 10);
        machine->write_memory(code_address, std::array{instruction, instruction, 0xD4000001u});
        machine->reg(arm64_register::x5, mmio_address + 0x40);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x5), mmio_address + 0x50);
            std::array<uint8_t, 16> vector{};
            machine->read_register(arm64_register::q0, vector.data(), vector.size());
            EXPECT_TRUE(std::equal(vector.begin(), vector.end(), data.begin()));
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(reads, 2u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x5), mmio_address + 0x60);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
        std::array<uint8_t, 16> vector{};
        machine->read_register(arm64_register::q0, vector.data(), vector.size());
        EXPECT_TRUE(std::equal(vector.begin(), vector.end(), data.begin() + 16));
    }

    INSTANTIATE_TEST_SUITE_P(ElementSize, SeccompLd1Test, testing::Values(0u, 1u, 2u, 3u));

    TEST_P(SeccompByteLaneLoadTest, UpdatesOnlyTheSelectedByteLanesAndWritesBackAfterReading)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 8> data{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
        const auto count = GetParam();
        const auto registers = count == 2 ? std::array{31u, 0u, 0u, 0u} : std::array{30u, 31u, 0u, 1u};
        const uint32_t instruction = (count == 2 ? 0x0DFF0020u : 0x0DFF2020u) | registers[0];
        std::array<std::array<uint8_t, 16>, 32> expected{};
        const auto vector_register = [](const uint32_t index) {
            return static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::q0) + index);
        };
        for (uint32_t i = 0; i < expected.size(); ++i)
        {
            expected[i].fill(static_cast<uint8_t>(0xA0 + i));
            machine->write_register(vector_register(i), expected[i].data(), expected[i].size());
        }
        const auto expect_vectors = [&](const size_t offset) {
            for (uint32_t i = 0; i < count; ++i)
            {
                expected[registers[i]][0] = data[offset + i];
            }
            for (uint32_t i = 0; i < expected.size(); ++i)
            {
                std::array<uint8_t, 16> vector{};
                machine->read_register(vector_register(i), vector.data(), vector.size());
                EXPECT_EQ(vector, expected[i]) << "Q" << i;
            }
        };
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             ASSERT_LT(reads, 2u);
                                             ASSERT_EQ(size, count);
                                             EXPECT_EQ(offset, 0x40 + count * reads);
                                             EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 0x40 + count * reads);
                                             std::memcpy(output, data.data() + count * reads, size);
                                             ++reads;
                                         },
                                         {}));
        machine->write_memory(code_address, std::array{instruction, instruction, 0xD4000001u});
        machine->reg(arm64_register::x1, mmio_address + 0x40);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 0x40 + count);
            expect_vectors(0);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(reads, 2u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 0x40 + 2 * count);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
        expect_vectors(count);
    }

    INSTANTIATE_TEST_SUITE_P(RegisterCount, SeccompByteLaneLoadTest, testing::Values(2u, 4u));

    TEST_F(SeccompBackendTest, EveryMmioLoadReadsTheCallbackIncludingZeroRegisterAndSoftwareStep)
    {
        constexpr uint64_t mmio_address = 0x40000;
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             EXPECT_EQ(offset, reads == 0 ? 0u : 8u);
                                             ASSERT_EQ(size, sizeof(uint64_t));
                                             const uint64_t value = ++reads;
                                             std::memcpy(output, &value, sizeof(value));
                                         },
                                         {}));
        machine->write_memory(code_address, std::array{0xF8408420u, 0xF940003Fu, 0xF9400022u, 0xD4000001u});
        machine->reg(arm64_register::x1, mmio_address);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
            EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 8);
            EXPECT_EQ(machine->reg(arm64_register::pstate) & (1u << 21), 0u);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(reads, 3u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
        EXPECT_EQ(machine->reg(arm64_register::x2), 3u);
    }

    TEST_F(SeccompBackendTest, UnsupportedMmioInstructionsFailWithoutCallingCallbacks)
    {
        constexpr uint64_t mmio_address = 0x40000;
        ASSERT_TRUE(memory.allocate_mmio(
            mmio_address, 0x1000, [](uint64_t, void*, size_t) { ADD_FAILURE() << "Unexpected MMIO read"; },
            [](uint64_t, const void*, size_t) { ADD_FAILURE() << "Unexpected MMIO write"; }));
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface&, uint64_t) {
            ADD_FAILURE() << "Execution must stop at the unsupported MMIO instruction";
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto instruction : {0xF9000020u, 0xA9400820u, 0xC85F7C20u})
        {
            machine->write_memory(code_address, std::array{instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x1, mmio_address);
            EXPECT_THROW(machine->start(), std::runtime_error);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
        }
        constexpr uint64_t value = 1;
        EXPECT_FALSE(machine->try_write_memory(mmio_address, &value, sizeof(value)));
    }

}
#endif
