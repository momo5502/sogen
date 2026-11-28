#include "std_include.hpp"

#include <atomic>
#include <thread>

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 2> syscall_instruction{0x0F, 0x05};
        constexpr std::array<uint8_t, 2> jump_to_itself{0xEB, 0xFE};
        constexpr size_t rounds = 1000;
    }

    TEST(WhpStopTest, OneStopEndsARunGoingBackIntoTheGuest)
    {
        if (get_x86_64_emulator_backend_from_environment() != backend_type::whp)
        {
            GTEST_SKIP() << "The run loop this tests is WHP's (EMULATOR_WHP=1)";
        }

        const auto machine = create_x86_64_emulator_from_environment(1);
        memory_manager memory{*machine};
        const auto code = memory.allocate_memory(0x1000, memory_permission::read_exec);
        const auto stack = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(0u, code);
        const auto spin = code + syscall_instruction.size();
        machine->write_memory(code, syscall_instruction.data(), syscall_instruction.size());
        machine->write_memory(spin, jump_to_itself.data(), jump_to_itself.size());

        std::atomic_bool exited{false};
        auto* hook = machine->hook_instruction(x86_hookable_instructions::syscall, [&](cpu_interface&, uint64_t) {
            exited = true;
            return instruction_hook_continuation::skip_instruction;
        });

        auto& cpu = machine->get_cpu(0);
        for (size_t round = 0; round < rounds; ++round)
        {
            cpu.reg(x86_register::rip, code);
            cpu.reg(x86_register::rsp, stack + 0x800);
            exited = false;
            std::atomic_bool finished{false};
            std::string failure{};
            std::thread runner([&] {
                try
                {
                    cpu.start();
                }
                catch (const std::exception& e)
                {
                    failure = e.what();
                }
                finished = true;
            });

            while (!exited && !finished)
            {
                std::this_thread::yield();
            }
            cpu.stop();
            runner.join();

            ASSERT_EQ("", failure) << "round " << round;
            ASSERT_EQ(spin, cpu.reg(x86_register::rip)) << "round " << round;
        }

        machine->delete_hook(hook);
    }
}
