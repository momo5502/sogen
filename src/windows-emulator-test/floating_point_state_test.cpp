#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        // A distinct value in every byte a register holds; an x87 register holds 10.
        std::array<std::byte, 16> pattern(const size_t index, const size_t size)
        {
            std::array<std::byte, 16> value{};
            for (size_t i = 0; i < size; ++i)
            {
                value[i] = static_cast<std::byte>(index * value.size() + i + 1);
            }

            return value;
        }

        constexpr size_t x87_register_size = 10;

        x86_64_cpu::floating_point_state distinct_state()
        {
            x86_64_cpu::floating_point_state state{};
            state.control_word = 0x37F; // with bit 6, which the processor holds set
            state.status_word = 0x0700; // C0, C1 and C2 with TOP 0
            state.tag_word = 0x5A;      // registers 1, 3, 4 and 6 in use
            state.mxcsr = 0x1FA0;       // every exception masked, PE set
            for (size_t i = 0; i < state.st.size(); ++i)
            {
                state.st[i] = pattern(i, x87_register_size);
            }

            for (size_t i = 0; i < state.xmm.size(); ++i)
            {
                state.xmm[i] = pattern(state.st.size() + i, state.xmm[i].size());
            }

            return state;
        }

        std::array<std::byte, 16> read_vector(x86_64_cpu& cpu, const x86_register first, const size_t index)
        {
            std::array<std::byte, 16> value{};
            cpu.read_register(static_cast<x86_register>(static_cast<size_t>(first) + index), value.data(), value.size());
            return value;
        }
    }

    // The state in one call is the same registers a backend gives one at a time.
    TEST(FloatingPointStateTest, OneCallMatchesTheRegistersOneByOne)
    {
        auto emu = create_empty_emulator();
        auto& cpu = emu.vcpu(0).x64_cpu();
        if (!cpu.supports_x87_registers())
        {
            GTEST_SKIP() << "The " << emu.emu().get_name() << " backend has no x87 registers";
        }
        const auto written = distinct_state();

        cpu.write_floating_point_state(written);
        EXPECT_EQ(written.control_word, cpu.reg<uint16_t>(x86_register::fpcw));
        EXPECT_EQ(written.status_word, cpu.reg<uint16_t>(x86_register::fpsw));
        EXPECT_EQ(written.tag_word, cpu.reg<uint16_t>(x86_register::fptag));
        EXPECT_EQ(written.mxcsr, cpu.reg<uint32_t>(x86_register::mxcsr));
        for (size_t i = 0; i < written.st.size(); ++i)
        {
            EXPECT_EQ(written.st[i], read_vector(cpu, x86_register::st0, i)) << "ST" << i;
        }

        for (size_t i = 0; i < written.xmm.size(); ++i)
        {
            EXPECT_EQ(written.xmm[i], read_vector(cpu, x86_register::xmm0, i)) << "XMM" << i;
        }

        x86_64_cpu::floating_point_state read{};
        cpu.read_floating_point_state(read);
        EXPECT_EQ(written.control_word, read.control_word);
        EXPECT_EQ(written.status_word, read.status_word);
        EXPECT_EQ(written.tag_word, read.tag_word);
        EXPECT_EQ(written.mxcsr, read.mxcsr);
        EXPECT_EQ(written.st, read.st);
        EXPECT_EQ(written.xmm, read.xmm);
    }
}
