#include <backend_selection.hpp>
#include <memory_manager.hpp>

#include <gtest/gtest.h>

#include <array>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t hook_address = 0x10000;
        constexpr uint64_t outgoing_address = 0x10100;
        constexpr uint64_t restored_address = 0x10200;

        class UnicornRestartStateTest : public testing::Test
        {
          protected:
            std::unique_ptr<x86_64_emulator> machine{};
            std::unique_ptr<memory_manager> memory{};

            void SetUp() override
            {
#ifdef _M_ARM64
                GTEST_SKIP() << "Unicorn backend is not available on Windows ARM64";
#else
                machine = create_x86_64_emulator(backend_type::unicorn);
                memory = std::make_unique<memory_manager>(*machine);
                ASSERT_TRUE(memory->allocate_memory(hook_address, 0x1000, memory_permission::all));
                machine->write_memory(hook_address, std::array<uint8_t, 1>{0x90});
                machine->write_memory(outgoing_address, std::array<uint8_t, 5>{0xB8, 1, 0, 0, 0});
                machine->write_memory(restored_address, std::array<uint8_t, 5>{0xB8, 42, 0, 0, 0});
                machine->reg(x86_register::rip, restored_address);
#endif
            }

            void pend_outgoing_restart()
            {
                auto* hook = machine->hook_memory_execution(
                    hook_address, [&](cpu_interface&, uint64_t) { machine->reg(x86_register::rip, outgoing_address); });
                machine->reg(x86_register::rip, hook_address);
                machine->start(1);
                machine->delete_hook(hook);
                ASSERT_TRUE(machine->has_violation());
                ASSERT_EQ(machine->read_instruction_pointer(), outgoing_address);
            }

            void expect_restored_path()
            {
                machine->start(1);
                EXPECT_EQ(machine->reg(x86_register::rax), 42u);
                EXPECT_EQ(machine->read_instruction_pointer(), restored_address + 5);
            }
        };
    }

    TEST_F(UnicornRestartStateTest, RestoreRegistersDiscardsOutgoingRestart)
    {
        const auto registers = machine->save_registers();
        ASSERT_NO_FATAL_FAILURE(pend_outgoing_restart());
        machine->restore_registers(registers);
        expect_restored_path();
    }

    TEST_F(UnicornRestartStateTest, DeserializeStateDiscardsOutgoingRestart)
    {
        utils::buffer_serializer serializer{};
        machine->serialize_state(serializer, false);
        ASSERT_NO_FATAL_FAILURE(pend_outgoing_restart());
        utils::buffer_deserializer deserializer{serializer.get_buffer()};
        machine->deserialize_state(deserializer, false);
        expect_restored_path();
    }
}
