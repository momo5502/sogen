#include "std_include.hpp"

#include <atomic>
#include <thread>

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 2> rdtsc{0x0F, 0x31};
        constexpr std::array<uint8_t, 3> inc_qword_rip_relative{0x48, 0xFF, 0x05};
        constexpr uint8_t jmp_rel8 = 0xEB;
        constexpr size_t jmp_rel8_size = 2;
        constexpr uint64_t iteration_count_offset = 0x800;

        std::vector<uint8_t> counting_rdtsc_loop()
        {
            std::vector<uint8_t> loop{rdtsc.begin(), rdtsc.end()};
            loop.insert(loop.end(), inc_qword_rip_relative.begin(), inc_qword_rip_relative.end());
            const auto end_of_inc = static_cast<int32_t>(loop.size() + sizeof(int32_t));
            const auto displacement = static_cast<int32_t>(iteration_count_offset) - end_of_inc;
            loop.insert(loop.end(), reinterpret_cast<const uint8_t*>(&displacement),
                        reinterpret_cast<const uint8_t*>(&displacement) + sizeof(displacement));
            const auto end_of_jmp = static_cast<int32_t>(loop.size() + jmp_rel8_size);
            loop.push_back(jmp_rel8);
            loop.push_back(static_cast<uint8_t>(-end_of_jmp));
            return loop;
        }
    }

    TEST(RdtscExitSwitchTest, HookingWhileVcpusRunTheGuestTurnsTheExitOnAndOff)
    {
        if (get_x86_64_emulator_backend_from_environment() != backend_type::whp)
        {
            GTEST_SKIP() << "RDTSC exits that follow the hooks at run time are WHP's (EMULATOR_WHP=1)";
        }

        constexpr size_t vcpus = 2;
        constexpr size_t switches = 50;
        const auto machine = create_x86_64_emulator_from_environment(vcpus);
        memory_manager memory{*machine};
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(0u, code);
        const auto loop = counting_rdtsc_loop();
        machine->write_memory(code, loop.data(), loop.size());
        const auto iterations = [&] {
            uint64_t count{};
            machine->read_memory(code + iteration_count_offset, &count, sizeof(count));
            return count;
        };

        const auto initial = machine->get_cpu(0).save_registers();
        for (size_t i = 0; i < vcpus; ++i)
        {
            auto& cpu = machine->get_cpu(i);
            cpu.restore_registers(initial);
            cpu.reg(x86_register::rip, code);
        }

        // Everything the runners and the hook's callbacks touch is declared before join_at_exit, so an early return
        // joins them first.
        std::vector<std::string> failures(vcpus);
        std::atomic_bool failed{false};
        std::array<std::atomic_bool, vcpus> finished{};
        std::atomic<uint64_t> rdtsc_exits{0};
        std::vector<std::thread> runners{};
        for (size_t i = 0; i < vcpus; ++i)
        {
            runners.emplace_back([&, i] {
                try
                {
                    machine->get_cpu(i).start();
                }
                catch (const std::exception& e)
                {
                    failures[i] = e.what();
                    failed = true;
                }
                finished[i] = true;
            });
        }
        bool joined = false;
        const auto join_vcpus = [&] {
            if (std::exchange(joined, true))
            {
                return;
            }
            // A stop that reaches a vCPU just before it enters the guest is lost, so it is sent again until the run ends.
            for (size_t i = 0; i < vcpus; ++i)
            {
                while (!finished[i])
                {
                    machine->get_cpu(i).stop();
                    std::this_thread::yield();
                }
            }
            for (auto& runner : runners)
            {
                runner.join();
            }
        };
        const auto join_at_exit = utils::finally(join_vcpus);

        const auto wait_for = [&](const auto& condition) {
            while (!condition() && !failed)
            {
                std::this_thread::yield();
            }
        };
        const auto wait_for_more_iterations = [&] {
            const auto from = iterations();
            wait_for([&] { return iterations() > from; });
        };
        wait_for_more_iterations();

        for (size_t round = 0; round < switches && !failed; ++round)
        {
            SCOPED_TRACE(testing::Message() << "round " << round);
            emulator_hook* hook{};
            ASSERT_NO_THROW(hook = machine->hook_instruction(x86_hookable_instructions::rdtsc, [&](cpu_interface&, uint64_t) {
                ++rdtsc_exits;
                return instruction_hook_continuation::run_instruction;
            }));
            const auto exits_before = rdtsc_exits.load();
            wait_for([&] { return rdtsc_exits.load() > exits_before; });

            ASSERT_NO_THROW(machine->delete_hook(hook));
            wait_for_more_iterations();
        }

        join_vcpus();
        for (size_t i = 0; i < vcpus; ++i)
        {
            EXPECT_EQ("", failures[i]) << "vCPU " << i << " stopped";
        }
    }
}
