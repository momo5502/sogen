#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 5> counting_writer{0x48, 0xFF, 0x03, 0xEB, 0xFB}; // inc qword ptr [rbx]; jmp back
    }

    TEST(WritePermissionTest, ARunningWriterFaultsRightAfterThePageIsReadOnly)
    {
        if (!create_x86_64_emulator_from_environment()->supports_multiple_vcpus())
        {
            GTEST_SKIP() << "The x86-64 backend of this host runs a single vCPU";
        }

        for (const bool toggled : {false, true})
        {
            const auto machine = create_x86_64_emulator_from_environment(2);
            memory_manager memory{*machine};
            const auto code = memory.allocate_memory(0x1000, memory_permission::read_exec);
            const auto data = memory.allocate_memory(0x1000, memory_permission::read_write);
            const auto stack = memory.allocate_memory(0x1000, memory_permission::read_write);
            machine->write_memory(code, counting_writer.data(), counting_writer.size());
            if (toggled)
            {
                ASSERT_TRUE(memory.protect_memory(data, 0x1000, memory_permission::read));
                ASSERT_TRUE(memory.protect_memory(data, 0x1000, memory_permission::read_write));
            }

            const auto count = [&] {
                uint64_t value{};
                machine->read_memory(data, &value, sizeof(value));
                return value;
            };
            std::atomic_bool faulted{false};
            std::atomic<uint64_t> count_at_fault{0};
            auto* violation = machine->hook_memory_violation(
                [&](cpu_interface&, const uint64_t address, size_t, const memory_operation operation, memory_violation_type) {
                    if (address >= data && address < data + 0x1000 && operation == memory_operation::write && !faulted)
                    {
                        count_at_fault = count();
                        faulted = true;
                    }
                    return memory_violation_continuation::stop;
                });

            auto& cpu = machine->get_cpu(0);
            cpu.reg(x86_register::rip, code);
            cpu.reg(x86_register::rsp, stack + 0x800);
            cpu.reg(x86_register::rbx, data);
            std::atomic_bool finished{false};
            std::string failure{};
            std::thread writer([&] {
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

            while (count() == 0 && !finished)
            {
                std::this_thread::yield();
            }
            EXPECT_TRUE(memory.protect_memory(data, 0x1000, memory_permission::read));
            const auto count_at_return = count();

            while (!faulted && !finished && count() == count_at_return)
            {
                std::this_thread::yield();
            }
            while (!finished)
            {
                cpu.stop(); // a stop that meets the run's start is lost, so it is asked again
                std::this_thread::yield();
            }
            writer.join();

            EXPECT_EQ("", failure);
            EXPECT_TRUE(faulted) << (toggled ? "toggled" : "native");
            EXPECT_EQ(count_at_return, count_at_fault.load()) << (toggled ? "toggled" : "native");
            machine->delete_hook(violation);
        }
    }
}
