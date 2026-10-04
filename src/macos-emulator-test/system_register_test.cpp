#include <gtest/gtest.h>

#include <backend_selection.hpp>
#include <unicorn_arm64_emulator.hpp>

#include <array>

namespace
{
    constexpr auto cntfrq_el0 = std::to_array<uint32_t>({3, 3, 14, 0, 0});
    constexpr auto cntpct_el0 = std::to_array<uint32_t>({3, 3, 14, 0, 1});
    constexpr auto id_aa64isar1_el1 = std::to_array<uint32_t>({3, 0, 0, 6, 1});

    TEST(Arm64SystemRegister, CounterFrequencyIsNonZero)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();

        const auto frequency = emu->read_system_register(cntfrq_el0[0], cntfrq_el0[1], cntfrq_el0[2], cntfrq_el0[3], cntfrq_el0[4]);
        EXPECT_NE(frequency, 0u);
    }

    TEST(Arm64SystemRegister, PhysicalCounterAdvances)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        constexpr uint64_t code_base = 0x100000000ULL;
        emu->map_memory(code_base, 0x4000, sogen::memory_permission::all);

        constexpr auto nops = std::to_array<uint32_t>({0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F});
        emu->write_memory(code_base, nops.data(), sizeof(nops));
        emu->reg(sogen::arm64_register::pc, code_base);

        const auto before = emu->read_system_register(cntpct_el0[0], cntpct_el0[1], cntpct_el0[2], cntpct_el0[3], cntpct_el0[4]);
        emu->start(4);
        const auto after = emu->read_system_register(cntpct_el0[0], cntpct_el0[1], cntpct_el0[2], cntpct_el0[3], cntpct_el0[4]);

        EXPECT_GE(after, before);
    }

    // QEMU's "max" model advertises the architected PAuth algorithm only: APA=1, API=0
    // (deps/unicorn/qemu/target/arm/cpu64.c:239-240). Real Apple silicon reports the opposite
    // (API=4, GPI=1), so sysctl must answer PAuth queries from emulator-side tables, not from here.
    constexpr uint64_t id_aa64isar1_apa = 0xFULL << 4;
    constexpr uint64_t id_aa64isar1_api = 0xFULL << 8;

    TEST(Arm64SystemRegister, IdRegisterReadsBackThroughTheInterface)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();

        const auto isar1 = emu->read_system_register(id_aa64isar1_el1[0], id_aa64isar1_el1[1], id_aa64isar1_el1[2], id_aa64isar1_el1[3],
                                                     id_aa64isar1_el1[4]);
        EXPECT_NE(isar1, 0u) << "read_raw_register's memset would have zeroed the CP_REG encoding";
        EXPECT_NE(isar1 & id_aa64isar1_apa, 0u) << "APA field expected non-zero under UC_CPU_ARM64_MAX";
        EXPECT_EQ(isar1 & id_aa64isar1_api, 0u);
    }
}
