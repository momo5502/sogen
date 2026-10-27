#include "emulation_test_utils.hpp"

#include <array>

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__amd64__))
#include <unistd.h>
#endif

namespace sogen::test
{
#if !defined(_M_ARM64)
#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__amd64__))
    TEST(EmulationTest, KvmExecutesXrstor)
    {
        if (::access("/dev/kvm", R_OK | W_OK) != 0 || !__builtin_cpu_supports("xsave"))
        {
            GTEST_SKIP();
        }

        auto machine = create_x86_64_emulator(backend_type::kvm);
        constexpr uint64_t code_address = 0x10000;
        constexpr uint64_t xsave_address = 0x20000;
        constexpr std::array<uint8_t, 5> code{0x0F, 0xAE, 0x29, 0x0F, 0x05};
        memory_manager memory{*machine};
        ASSERT_TRUE(memory.allocate_memory(code_address, 0x1000, memory_permission::all));
        ASSERT_TRUE(memory.allocate_memory(xsave_address, 0x1000, memory_permission::read_write));
        machine->write_memory(code_address, code.data(), code.size());

        auto& cpu = machine->get_cpu(0);
        cpu.reg(x86_register::rip, code_address);
        cpu.reg(x86_register::rcx, xsave_address);
        cpu.reg(x86_register::rax, uint64_t{0});
        cpu.reg(x86_register::rdx, uint64_t{0});

        bool reached_syscall = false;
        machine->hook_instruction(x86_hookable_instructions::syscall, [&](cpu_interface& acting, uint64_t) {
            reached_syscall = true;
            acting.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        cpu.start();

        EXPECT_TRUE(reached_syscall);
    }
#endif

    TEST(EmulationTest, IntegerDivideErrorsReachTheInterruptHook)
    {
        auto machine = create_x86_64_emulator_from_environment();
        constexpr uint64_t address = 0x10000;
        constexpr std::array<uint8_t, 2> code{0xF7, 0xF1};
        memory_manager memory{*machine};
        ASSERT_TRUE(memory.allocate_memory(address, 0x1000, memory_permission::all));
        machine->write_memory(address, code.data(), code.size());
        auto& cpu = machine->get_cpu(0);
        cpu.reg(x86_register::rip, address);
        cpu.reg(x86_register::rax, uint64_t{10});
        cpu.reg(x86_register::rdx, uint64_t{0});
        cpu.reg(x86_register::rcx, uint64_t{0});
        int interrupt = -1;
        machine->hook_interrupt([&](cpu_interface& acting, const int vector) {
            interrupt = vector;
            acting.stop();
        });

        cpu.start(1);

        EXPECT_EQ(interrupt, 0);
        EXPECT_EQ(cpu.reg<uint64_t>(x86_register::rip), address);
    }
#endif

    TEST(EmulationTest, BasicEmulationWorks)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
    }

    TEST(EmulationTest, CountedEmulationWorks)
    {
        constexpr auto count = 200000;

        auto emu = create_sample_emulator();
        emu.start(count);

        ASSERT_EQ(emu.get_executed_instructions(), count);
    }

    TEST(EmulationTest, CountedEmulationIsAccurate)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        const auto executedInstructions = emu.get_executed_instructions();

        auto new_emu = create_sample_emulator();

        constexpr auto offset = 1;
        const auto instructionsToExecute = executedInstructions - offset;

        new_emu.start(static_cast<size_t>(instructionsToExecute));

        ASSERT_EQ(new_emu.get_executed_instructions(), instructionsToExecute);
        ASSERT_NOT_TERMINATED(new_emu);

        new_emu.start(offset);

        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);
        ASSERT_EQ(new_emu.get_executed_instructions(), executedInstructions);
    }

    TEST(EmulationTest, SocketActivityReportsGuestAddresses)
    {
        std::vector<std::pair<network::socket_activity, std::string>> activities{};

        emulator_callbacks callbacks{};
        callbacks.on_socket_activity = [&](const network::socket_activity activity, const network::address& address) {
            activities.emplace_back(activity, address.to_string());
        };

        auto emu = create_sample_emulator({.use_relative_time = true}, {}, std::move(callbacks));
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        const auto contains = [&](const network::socket_activity activity, const std::string_view address) {
            return std::ranges::find(activities, std::pair{activity, std::string(address)}) != activities.end();
        };

        EXPECT_TRUE(contains(network::socket_activity::bind, "127.0.0.1:28970"));
        EXPECT_TRUE(contains(network::socket_activity::sendto, "127.0.0.1:28970"));
    }

    TEST(EmulationTest, RelativeApplicationMachineResolvesAgainstTheWorkingDirectory)
    {
        const emulator_settings settings{.emulation_root = get_emulator_root()};
        const application_settings app_settings{.application = "test-sample.exe", .working_directory = "C:\\"};

        EXPECT_EQ(get_application_machine(settings, app_settings), PEMachineType::AMD64);
    }

    TEST(EmulationTest, ApplicationMachineUsesFileAndDirectoryMountsForRelativePaths)
    {
        const auto root = get_emulator_root();
        emulator_settings settings{.emulation_root = root};
        const auto expected = get_application_machine(settings, application_settings{.application = "C:\\test-sample.exe"});
        const auto sample = root / "filesys" / "c" / "test-sample.exe";
        const std::array mounts{
            mount_spec{.host_path = sample, .guest_path = windows_path{"Z:\\mounted\\test-sample.exe"}},
            mount_spec{.host_path = sample.parent_path(), .guest_path = windows_path{"Z:\\mounted"}},
        };
        const application_settings app_settings{.application = "test-sample.exe", .working_directory = "Z:\\mounted"};

        for (const auto& mount : mounts)
        {
            settings.mounts = {mount};
            EXPECT_EQ(get_application_machine(settings, app_settings), expected);
        }
    }
} // namespace sogen::test
