#include "emulation_test_utils.hpp"

#if defined(__ANDROID__) || defined(__APPLE__)
#include <memory_manager.hpp>

#include <array>
#endif

#ifdef __APPLE__
#include <cstring>

#include <sys/mman.h>
#endif

namespace sogen::test
{
    namespace
    {
        std::unique_ptr<x86_64_emulator> try_create_fex_emulator()
        {
            try
            {
                return create_x86_64_emulator(backend_type::fex);
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

    }

    TEST(FexRegisterTest, ExtendedGprSubRegisterReadsAliasFullRegister)
    {
        const auto emu = try_create_fex_emulator();
        if (!emu)
        {
            GTEST_SKIP() << "FEX backend is not available";
        }

        emu->reg(x86_register::r9, 0x1122334455667788ULL);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::r9d), 0x55667788U);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::r9w), 0x7788U);
        EXPECT_EQ(emu->reg<uint8_t>(x86_register::r9b), 0x88U);

        emu->reg(x86_register::r15, 0xCAFEBABE00C0FFEEULL);
        EXPECT_EQ(emu->reg<uint32_t>(x86_register::r15d), 0x00C0FFEEU);
        EXPECT_EQ(emu->reg<uint16_t>(x86_register::r15w), 0xFFEEU);
        EXPECT_EQ(emu->reg<uint8_t>(x86_register::r15b), 0xEEU);
    }

#ifdef __ANDROID__
    TEST(FexMemoryViolationTest, PlainGuestStoreToReadOnlyMemoryIsWrite)
    {
        const auto emu = try_create_fex_emulator();
        if (!emu)
        {
            GTEST_SKIP() << "FEX backend is not available";
        }

        memory_manager memory{*emu};
        constexpr size_t page_size = 0x1000;
        const uint64_t code = memory.allocate_memory(page_size, memory_permission::read_write);
        const uint64_t target = memory.allocate_memory(page_size, memory_permission::read_write);
        const uint64_t gdt = memory.allocate_memory(page_size, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(target, 0u);
        ASSERT_NE(gdt, 0u);

        constexpr uint16_t long_mode_code_selector = 0x08;
        constexpr uint64_t long_mode_code_descriptor = 0x00AF9B000000FFFF;
        emu->write_memory<uint64_t>(gdt + long_mode_code_selector, long_mode_code_descriptor);
        emu->load_gdt(gdt, page_size);
        emu->reg<uint16_t>(x86_register::cs, long_mode_code_selector);

        // mov [rax], rbx; hlt. The translated store is an ordinary guest store; the Android fault
        // classifier must not depend on the deliberately narrow STLR-family emulation decoder.
        constexpr std::array<uint8_t, 4> guest_code = {0x48, 0x89, 0x18, 0xF4};
        memory.write_memory(code, guest_code.data(), guest_code.size());
        ASSERT_TRUE(memory.protect_memory(code, page_size, memory_permission::read_exec));
        ASSERT_TRUE(memory.protect_memory(target, page_size, memory_permission::read));

        bool observed = false;
        uint64_t observed_address = 0;
        memory_operation observed_operation = memory_operation::read;
        memory_violation_type observed_type = memory_violation_type::unmapped;
        emu->hook_memory_violation(
            [&](cpu_interface& cpu, uint64_t address, size_t, memory_operation operation, memory_violation_type type) {
                observed = true;
                observed_address = address;
                observed_operation = operation;
                observed_type = type;
                cpu.stop();
                return memory_violation_continuation::stop;
            });

        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rax, target);
        emu->reg(x86_register::rbx, 0x1122334455667788ULL);
        emu->start(0);

        EXPECT_TRUE(observed);
        EXPECT_EQ(observed_address, target);
        EXPECT_EQ(observed_operation, memory_operation::write);
        EXPECT_EQ(observed_type, memory_violation_type::protection);
    }

    TEST(FexMemoryViolationTest, UnalignedLoadCrossingIntoUnmappedMemoryIsGuestViolation)
    {
        const auto emu = try_create_fex_emulator();
        if (!emu)
        {
            GTEST_SKIP() << "FEX backend is not available";
        }

        memory_manager memory{*emu};
        constexpr size_t page_size = 0x1000;
        const uint64_t code = memory.allocate_memory(page_size, memory_permission::read_write);
        const uint64_t target = memory.allocate_memory(page_size * 2, memory_permission::read_write);
        const uint64_t gdt = memory.allocate_memory(page_size, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(target, 0u);
        ASSERT_NE(gdt, 0u);
        ASSERT_TRUE(memory.decommit_memory(target + page_size, page_size));

        constexpr uint16_t long_mode_code_selector = 0x08;
        constexpr uint64_t long_mode_code_descriptor = 0x00AF9B000000FFFF;
        emu->write_memory<uint64_t>(gdt + long_mode_code_selector, long_mode_code_descriptor);
        emu->load_gdt(gdt, page_size);
        emu->reg<uint16_t>(x86_register::cs, long_mode_code_selector);

        constexpr std::array<uint8_t, 4> guest_code = {0x48, 0x8B, 0x18, 0xF4};
        memory.write_memory(code, guest_code.data(), guest_code.size());
        ASSERT_TRUE(memory.protect_memory(code, page_size, memory_permission::read_exec));

        bool observed = false;
        uint64_t observed_address = 0;
        size_t observed_size = 0;
        memory_operation observed_operation = memory_operation::write;
        memory_violation_type observed_type = memory_violation_type::protection;
        emu->hook_memory_violation(
            [&](cpu_interface& cpu, uint64_t address, size_t size, memory_operation operation, memory_violation_type type) {
                observed = true;
                observed_address = address;
                observed_size = size;
                observed_operation = operation;
                observed_type = type;
                cpu.stop();
                return memory_violation_continuation::stop;
            });

        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rax, target + page_size - 5);
        emu->start(0);

        EXPECT_TRUE(observed);
        EXPECT_EQ(observed_address, target + page_size - 5);
        EXPECT_EQ(observed_size, 8u);
        EXPECT_EQ(observed_operation, memory_operation::read);
        EXPECT_EQ(observed_type, memory_violation_type::unmapped);
    }
#endif

#ifdef __APPLE__
    TEST(FexMemoryViolationTest, MisalignedAtomicOnHostAliasedMemoryIsEmulated)
    {
        const auto emu = try_create_fex_emulator();
        if (!emu)
        {
            GTEST_SKIP() << "FEX backend is not available";
        }

        constexpr size_t host_page_size = 0x4000;
        void* const host_buffer = ::mmap(nullptr, host_page_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        ASSERT_NE(host_buffer, MAP_FAILED);

        memory_manager memory{*emu};
        memory.reserve_host_memory_ranges();
        constexpr size_t page_size = 0x1000;
        const uint64_t code = memory.allocate_memory(page_size, memory_permission::read_write);
        const uint64_t gdt = memory.allocate_memory(page_size, memory_permission::read_write);
        const uint64_t target = memory.allocate_host_memory(host_page_size, host_buffer, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(gdt, 0u);
        ASSERT_NE(target, 0u);

        constexpr uint16_t long_mode_code_selector = 0x08;
        constexpr uint64_t long_mode_code_descriptor = 0x00AF9B000000FFFF;
        emu->write_memory<uint64_t>(gdt + long_mode_code_selector, long_mode_code_descriptor);
        emu->load_gdt(gdt, page_size);
        emu->reg<uint16_t>(x86_register::cs, long_mode_code_selector);

        // lock xadd [rax], rbx; hlt. The 8-byte operand straddles a cache line, which the host can only
        // handle by emulating the atomic.
        constexpr std::array<uint8_t, 6> guest_code = {0xF0, 0x48, 0x0F, 0xC1, 0x18, 0xF4};
        memory.write_memory(code, guest_code.data(), guest_code.size());
        ASSERT_TRUE(memory.protect_memory(code, page_size, memory_permission::read_exec));

        constexpr uint64_t operand_offset = 0x3C;
        const auto operand = static_cast<uint8_t*>(host_buffer) + operand_offset;
        const uint64_t initial_value = 0x10;
        std::memcpy(operand, &initial_value, sizeof(initial_value));

        bool violation_observed = false;
        emu->hook_memory_violation([&](cpu_interface& cpu, uint64_t, size_t, memory_operation, memory_violation_type) {
            violation_observed = true;
            cpu.stop();
            return memory_violation_continuation::stop;
        });
        emu->hook_interrupt([](cpu_interface& cpu, int) { cpu.stop(); });

        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rax, target + operand_offset);
        emu->reg(x86_register::rbx, 1);
        emu->start(0);

        uint64_t final_value{};
        std::memcpy(&final_value, operand, sizeof(final_value));

        EXPECT_FALSE(violation_observed);
        EXPECT_EQ(final_value, initial_value + 1);
        EXPECT_EQ(emu->reg(x86_register::rbx), initial_value);

        ::munmap(host_buffer, host_page_size);
    }
#endif

    TEST(FexRegisterTest, ExtendedGprSubRegisterWrites)
    {
        const auto emu = try_create_fex_emulator();
        if (!emu)
        {
            GTEST_SKIP() << "FEX backend is not available";
        }

        emu->reg(x86_register::r9, 0x1122334455667788ULL);

        emu->reg<uint8_t>(x86_register::r9b, 0xAA);
        EXPECT_EQ(emu->reg(x86_register::r9), 0x11223344556677AAULL);

        emu->reg<uint16_t>(x86_register::r9w, 0xBBCC);
        EXPECT_EQ(emu->reg(x86_register::r9), 0x112233445566BBCCULL);

        emu->reg<uint32_t>(x86_register::r9d, 0xDDEEFF00U);
        EXPECT_EQ(emu->reg(x86_register::r9), 0x00000000DDEEFF00ULL);
    }
} // namespace sogen::test
