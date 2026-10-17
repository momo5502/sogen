#include <memory_manager.hpp>
#include <whp_arm64_emulator.hpp>
#include <backend_selection.hpp>

#include <gtest/gtest.h>
#include <unicorn/unicorn.h>

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

namespace sogen::whp::test
{
    static_assert(static_cast<int>(arm64_register::x29) == UC_ARM64_REG_X29);
    static_assert(static_cast<int>(arm64_register::x30) == UC_ARM64_REG_X30);
    static_assert(static_cast<int>(arm64_register::x0) == UC_ARM64_REG_X0);
    static_assert(static_cast<int>(arm64_register::x28) == UC_ARM64_REG_X28);
    static_assert(static_cast<int>(arm64_register::w0) == UC_ARM64_REG_W0);
    static_assert(static_cast<int>(arm64_register::w30) == UC_ARM64_REG_W30);
    static_assert(static_cast<int>(arm64_register::q0) == UC_ARM64_REG_Q0);
    static_assert(static_cast<int>(arm64_register::q31) == UC_ARM64_REG_Q31);
    static_assert(static_cast<int>(arm64_register::v0) == UC_ARM64_REG_V0);
    static_assert(static_cast<int>(arm64_register::v31) == UC_ARM64_REG_V31);
    static_assert(static_cast<int>(arm64_register::pc) == UC_ARM64_REG_PC);
    static_assert(static_cast<int>(arm64_register::tpidr_el0) == UC_ARM64_REG_TPIDR_EL0);
    static_assert(static_cast<int>(arm64_register::tpidrro_el0) == UC_ARM64_REG_TPIDRRO_EL0);
    static_assert(static_cast<int>(arm64_register::pstate) == UC_ARM64_REG_PSTATE);
    static_assert(static_cast<int>(arm64_register::fpcr) == UC_ARM64_REG_FPCR);
    static_assert(static_cast<int>(arm64_register::fpsr) == UC_ARM64_REG_FPSR);

    namespace
    {
        constexpr uint64_t code_address = 0x10000;
        constexpr uint64_t data_address = 0x20000;
        constexpr uint64_t stack_address = 0x30000;

        constexpr uint32_t svc(const uint16_t id)
        {
            return 0xD4000001 | (static_cast<uint32_t>(id) << 5);
        }

        class WhpArm64Test : public testing::Test
        {
          protected:
            std::unique_ptr<arm64_emulator> machine = sogen::create_arm64_emulator(backend_type::whp);
            memory_manager memory{*machine};

            void SetUp() override
            {
                ASSERT_TRUE(memory.allocate_memory(code_address, 0x1000, memory_permission::all));
                ASSERT_TRUE(memory.allocate_memory(data_address, 0x1000, memory_permission::read_write));
                ASSERT_TRUE(memory.allocate_memory(stack_address, 0x1000, memory_permission::read_write));
                machine->reg(arm64_register::pc, code_address);
                machine->reg(arm64_register::sp, stack_address + 0xFF0);
            }

            template <size_t N>
            void load_code(const std::array<uint32_t, N>& code)
            {
                machine->write_memory(code_address, code);
            }
        };
    }

    TEST_F(WhpArm64Test, GuestFaultsStopAsReportedViolations)
    {
        load_code(std::array{0xD4200000u});
        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_TRUE(machine->has_violation());
        load_code(std::array{0xF9000100u});
        machine->reg(arm64_register::pc, code_address);
        machine->reg(arm64_register::x8, data_address);
        ASSERT_TRUE(memory.protect_memory(data_address, 0x1000, memory_permission::read));
        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_TRUE(machine->has_violation());
    }

    TEST_F(WhpArm64Test, CpuCanBeCanceledFromAnotherThread)
    {
        load_code(std::array{svc(1), 0x14000000u});
        std::promise<void> entered{};
        auto reached = entered.get_future();
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface&, uint64_t) {
            entered.set_value();
            return instruction_hook_continuation::skip_instruction;
        });
        auto running = std::async(std::launch::async, [&] { machine->start(); });
        const auto ready = reached.wait_for(std::chrono::seconds(2));
        if (ready == std::future_status::ready)
        {
            // Allow the VP to enter the infinite guest loop before testing cancellation.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        machine->stop();
        EXPECT_EQ(ready, std::future_status::ready);
        ASSERT_EQ(running.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_NO_THROW(running.get());
    }

    TEST_F(WhpArm64Test, HostAliasedCodeAndDataRemainCoherent)
    {
        constexpr uint64_t alias_address = 0x40000;
        std::vector<std::byte> backing(0x2000);
        const auto raw = reinterpret_cast<uintptr_t>(backing.data());
        auto* host = reinterpret_cast<std::byte*>((raw + 0xFFF) & ~uintptr_t{0xFFF});
        uint32_t instruction = svc(10);
        std::memcpy(host, &instruction, sizeof(instruction));
        ASSERT_TRUE(memory.allocate_host_memory_at(alias_address, 0x1000, host, memory_permission::all));
        int calls{};
        auto* hook = machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t id) {
            EXPECT_EQ(id, calls == 0 ? 10u : 11u);
            ++calls;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->reg(arm64_register::pc, alias_address);
        machine->start();
        instruction = svc(11);
        std::memcpy(host, &instruction, sizeof(instruction));
        machine->flush_host_memory_cache(host, sizeof(instruction));
        machine->reg(arm64_register::pc, alias_address);
        machine->start();
        EXPECT_EQ(calls, 2);
        const uint64_t pattern = 0x123456789ABCDEF0ull;
        machine->write_memory(alias_address + 0x100, pattern);
        uint64_t observed{};
        std::memcpy(&observed, host + 0x100, sizeof(observed));
        EXPECT_EQ(observed, pattern);
        EXPECT_TRUE(memory.release_memory(alias_address, 0x1000));
        machine->delete_hook(hook);
    }

    TEST_F(WhpArm64Test, HighUserAddressesDoNotNeedMatchingPhysicalAddresses)
    {
        // Above the 36-bit physical address width of some ARM64 hosts, as in user-mode dumps.
        constexpr uint64_t high_code = 0x7FFA4BAD0000;
        constexpr uint64_t high_data = high_code + 0x10000;
        ASSERT_TRUE(memory.allocate_memory(high_code, 0x1000, memory_permission::all));
        ASSERT_TRUE(memory.allocate_memory(high_data, 0x1000, memory_permission::read_write));
        machine->write_memory(high_code, std::array{0xF9000001u, svc(2)});
        auto* hook = machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto value : {0x1122334455667788ull, 0x8877665544332211ull})
        {
            machine->reg(arm64_register::pc, high_code);
            machine->reg(arm64_register::x0, high_data);
            machine->reg(arm64_register::x1, value);
            machine->start();
            EXPECT_EQ(machine->read_memory<uint64_t>(high_data), value);
            ASSERT_TRUE(memory.release_memory(high_data, 0x1000));
            ASSERT_TRUE(memory.allocate_memory(high_data, 0x1000, memory_permission::read_write));
        }
        machine->delete_hook(hook);
    }

    TEST_F(WhpArm64Test, ReleasedRegionsReturnTheirTranslationTables)
    {
        constexpr uint64_t base = 0x200000000;
        constexpr uint64_t window = 0x200000;
        for (uint64_t i = 0; i < 5000; ++i)
        {
            const auto address = base + i * window;
            ASSERT_TRUE(memory.allocate_memory(address, 0x1000, memory_permission::read_write)) << i;
            machine->write_memory(address, i);
            ASSERT_TRUE(memory.release_memory(address, 0x1000)) << i;
        }
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        machine->write_memory(base, uint64_t{42});
        load_code(std::array{0xF9400100u, svc(1)});
        auto* hook = machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->reg(arm64_register::pc, code_address);
        machine->reg(arm64_register::x8, base);
        machine->start();
        EXPECT_EQ(machine->reg(arm64_register::x0), 42u);
        machine->delete_hook(hook);
    }

    TEST_F(WhpArm64Test, UnsupportedFeaturesAndPrivateMappingsAreRejected)
    {
        EXPECT_THROW(whp::create_arm64_emulator(0), std::invalid_argument);
        EXPECT_THROW(whp::create_arm64_emulator(2), std::invalid_argument);
        EXPECT_THROW(sogen::create_arm64_emulator(backend_type::unicorn), std::runtime_error);
#if !SOGEN_ENABLE_RUST_CODE
        EXPECT_THROW(sogen::create_x86_64_emulator(), std::runtime_error);
        EXPECT_THROW(sogen::get_x86_64_emulator_backend_from_environment(), std::runtime_error);
        EXPECT_THROW(sogen::create_x86_64_emulator_from_environment(), std::runtime_error);
#endif
        EXPECT_THROW(machine->hook_memory_execution([](cpu_interface&, uint64_t) {}), std::runtime_error);
        // The private ranges are reported as host-reserved, so the memory manager never hands them to the guest.
        for (const auto address : {0xFFFF0000ull, 0x08000000ull, 0x08020000ull, 0xFE000000ull})
        {
            EXPECT_EQ(machine->reserved_host_ranges_in(address, 0x1000).size(), 1u) << std::hex << address;
            EXPECT_FALSE(memory.allocate_memory(address, 0x1000, memory_permission::all)) << std::hex << address;
        }
        EXPECT_TRUE(machine->reserved_host_ranges_in(0x09000000ull, 0x1000).empty());
    }
}
