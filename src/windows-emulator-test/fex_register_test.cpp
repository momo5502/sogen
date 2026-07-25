#include "emulation_test_utils.hpp"

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
                return nullptr;
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
