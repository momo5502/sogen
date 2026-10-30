#include "emulation_test_utils.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace sogen::test
{
    TEST(NullCallReportTest, ReportsFullReturnAddressOf64BitCaller)
    {
        auto emu = create_sample_emulator();

        try
        {
            emu.start(200000);
        }
        catch (const std::exception&)
        {
            GTEST_SKIP() << "Backend does not support exact instruction counts";
        }
        ASSERT_NOT_TERMINATED(emu);

        constexpr size_t page = 0x1000;
        const uint64_t code = emu.memory.allocate_memory(page, memory_permission::all);
        const uint64_t stack = emu.memory.allocate_memory(page, memory_permission::read_write);
        ASSERT_GT(code, UINT32_MAX);
        ASSERT_GT(stack, UINT32_MAX);

        auto& cpu = emu.x64_emu();
        constexpr std::array<uint8_t, 2> call_rax{0xFF, 0xD0};
        cpu.write_memory(code, call_rax.data(), call_rax.size());

        std::vector<std::string> errors{};
        emu.log.set_sink([&](const color c, const std::string_view message) {
            if (c == color::red)
            {
                errors.emplace_back(message);
            }
        });
        emu.callbacks.on_memory_violate = [&](uint64_t, uint64_t, memory_operation, memory_violation_type) { cpu.stop(); };

        cpu.reg(x86_register::rip, code);
        cpu.reg(x86_register::rsp, stack + (page / 2));
        cpu.reg(x86_register::rax, 0);
        cpu.start();

        std::ostringstream expected{};
        expected << "Null-pointer call to 0x0; caller return address 0x" << std::hex << (code + call_rax.size()) << " ";

        const auto reported =
            std::ranges::any_of(errors, [&](const std::string& error) { return error.find(expected.str()) != std::string::npos; });
        EXPECT_TRUE(reported) << "expected a report containing \"" << expected.str() << "\"";
    }
}
