#include <chrono>
#include <string>
#include <thread>

#if defined(__APPLE__) && defined(__aarch64__)
#include <cstring>
#include <sys/mman.h>
#endif
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <vector>

#include <unicorn/unicorn.h>
#include <arch_emulator.hpp>
#include <arm64_register.hpp>
#include <serialization.hpp>
#include <unicorn_arm64_emulator.hpp>
#include <backend_selection.hpp>

namespace
{
    TEST(Arm64BackendSelection, CreatesUnicornBackendByDefault)
    {
        const auto emu = sogen::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);
        EXPECT_EQ(emu->get_name(), "Unicorn Engine");
    }

    TEST(Arm64BackendSelection, RejectsUnavailableBackend)
    {
        EXPECT_THROW((void)sogen::create_arm64_emulator(sogen::backend_type::kvm), std::runtime_error);
    }
}

namespace
{
    TEST(Arm64Backend, UnicornOpensAarch64Engine)
    {
        uc_engine* uc{};
        ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
        ASSERT_NE(uc, nullptr);
        uc_close(uc);
    }

    TEST(Arm64Backend, UnicornSupportsAarch64Arch)
    {
        ASSERT_TRUE(uc_arch_supported(UC_ARCH_ARM64));
    }
}

namespace
{
    TEST(Arm64Register, MatchesUnicornEnumeration)
    {
        static_assert(static_cast<uint32_t>(sogen::arm64_register::end) == UC_ARM64_REG_ENDING);

        EXPECT_EQ(static_cast<int>(sogen::arm64_register::x0), UC_ARM64_REG_X0);
        EXPECT_EQ(static_cast<int>(sogen::arm64_register::x30), UC_ARM64_REG_X30);
        EXPECT_EQ(static_cast<int>(sogen::arm64_register::pc), UC_ARM64_REG_PC);
        EXPECT_EQ(static_cast<int>(sogen::arm64_register::sp), UC_ARM64_REG_SP);
        EXPECT_EQ(static_cast<int>(sogen::arm64_register::nzcv), UC_ARM64_REG_NZCV);
        EXPECT_EQ(static_cast<int>(sogen::arm64_register::tpidr_el0), UC_ARM64_REG_TPIDR_EL0);
        EXPECT_EQ(static_cast<int>(sogen::arm64_register::tpidrro_el0), UC_ARM64_REG_TPIDRRO_EL0);
        EXPECT_EQ(static_cast<int>(sogen::arm64_register::lr), UC_ARM64_REG_LR);
    }
}

namespace
{
    TEST(Arm64Traits, ExposesArchitecturalIdentity)
    {
        static_assert(sizeof(sogen::arm64_traits::pointer_type) == 8);
        static_assert(sogen::arm64_traits::instruction_pointer == sogen::arm64_register::pc);
        static_assert(sogen::arm64_traits::stack_pointer == sogen::arm64_register::sp);
        static_assert(std::is_same_v<sogen::arm64_traits::register_type, sogen::arm64_register>);

        EXPECT_EQ(static_cast<int>(sogen::arm64_hookable_instructions::svc), 1);
        EXPECT_EQ(static_cast<int>(sogen::arm64_hookable_instructions::brk), 2);
    }
}

namespace
{
    constexpr uint64_t code_base = 0x10000;
    constexpr uint64_t stack_base = 0x200000;
    constexpr size_t region_size = 0x10000;

    constexpr uint64_t sctlr_el1_pac_enables = (1ULL << 31) | (1ULL << 30) | (1ULL << 27) | (1ULL << 13);

    std::unique_ptr<sogen::arm64_64_emulator> make_emulator()
    {
        auto emu = sogen::unicorn::create_arm64_emulator();
        emu->map_memory(code_base, region_size, sogen::memory_permission::all);
        emu->map_memory(stack_base, region_size, sogen::memory_permission::read_write);
        emu->reg(sogen::arm64_register::sp, stack_base + region_size - 0x100);
        return emu;
    }

    TEST(UnicornArm64, ReadsAndWritesGeneralPurposeRegisters)
    {
        const auto emu = make_emulator();

        emu->reg(sogen::arm64_register::x0, 0x1122334455667788ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 0x1122334455667788ULL);

        emu->reg(sogen::arm64_register::x29, 0xFEEDFACEULL);
        emu->reg(sogen::arm64_register::x30, 0xDEADBEEFULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x30), 0xDEADBEEFULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x29), 0xFEEDFACEULL);

        // x29 and x30 sit *before* x0 in the enum, so a classifier that folds them into the x0..x28
        // range aliases them onto other registers. Writing them and then re-reading x0 is what catches
        // that; checking each register only against the value just written cannot.
        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 0x1122334455667788ULL);
    }

    TEST(UnicornArm64, ReadsAndWritesThreadPointer)
    {
        const auto emu = make_emulator();

        emu->set_thread_pointer(0x7000000000ULL);
        EXPECT_EQ(emu->get_thread_pointer(), 0x7000000000ULL);
    }

    TEST(UnicornArm64, ReadsAndWritesGuestMemory)
    {
        const auto emu = make_emulator();

        constexpr uint32_t value = 0xCAFEBABE;
        emu->write_memory(code_base, &value, sizeof(value));

        uint32_t read_back{};
        emu->read_memory(code_base, &read_back, sizeof(read_back));
        EXPECT_EQ(read_back, value);
    }

    TEST(UnicornArm64, ReportsItsName)
    {
        EXPECT_EQ(make_emulator()->get_name(), "Unicorn Engine");
    }
}

namespace
{
    void write_code(sogen::arm64_64_emulator& emu, const std::vector<uint32_t>& words)
    {
        emu.write_memory(code_base, words.data(), words.size() * sizeof(uint32_t));
        emu.reg(sogen::arm64_register::pc, code_base);
    }

    TEST(UnicornArm64Execution, ExecutesMovAndStopsAfterInstructionCount)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD2800540, // mov x0, #42
                             0xD503201F, // nop
                         });

        emu->start(2);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 42u);
        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), code_base + 8);
    }
}

namespace
{
    TEST(UnicornArm64Svc, InvokesInstructionHookOnSvc)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD2800090, // mov x16, #4
                             0xD4001001, // svc #0x80
                             0xD503201F, // nop
                         });

        uint64_t observed_syscall_number = 0;
        size_t hook_count = 0;

        emu->hook_instruction(sogen::arm64_hookable_instructions::svc, [&](sogen::cpu_interface&, uint64_t) {
            ++hook_count;
            observed_syscall_number = emu->reg(sogen::arm64_register::x16);
            return sogen::instruction_hook_continuation::run_instruction;
        });

        emu->start(3);

        EXPECT_EQ(hook_count, 1u);
        EXPECT_EQ(observed_syscall_number, 4u);
    }

    TEST(UnicornArm64Svc, FinalizedInstructionPointerIsLeftUnchanged)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD4001001, // svc #0x80
                         });

        emu->hook_instruction(sogen::arm64_hookable_instructions::svc, [&](sogen::cpu_interface&, uint64_t) {
            emu->reg(sogen::arm64_register::pc, code_base + 4);
            return sogen::instruction_hook_continuation::finalized_instruction_pointer;
        });

        emu->start(1);

        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), code_base + 4);
    }

    TEST(UnicornArm64Svc, InvokesInstructionHookOnBrk)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD4200000, // brk #0
                         });

        size_t hook_count = 0;
        emu->hook_instruction(sogen::arm64_hookable_instructions::brk, [&](sogen::cpu_interface&, uint64_t) {
            ++hook_count;
            return sogen::instruction_hook_continuation::skip_instruction;
        });

        emu->start(1);

        EXPECT_EQ(hook_count, 1u);
        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), code_base);
    }
}

namespace
{
    // QEMU advertises ID_AA64ISAR1.APA=1/API=0 (architected QARMA, cpu64.c:239-240), the exact inverse of
    // Apple silicon's API=4/GPI=1 (IMPDEF algorithm plus FPAC). arm64e binaries never carry a signature
    // across that boundary - every pointer they authenticate was signed by the same emulated keys - so
    // only the round-trip has to hold, not the specific PAC bits.
    TEST(UnicornArm64Pac, PaciaThenAutiaRoundTrips)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xDAC10020, // pacia x0, x1
                             0xDAC11020, // autia x0, x1
                         });

        constexpr uint64_t original = 0x0000000100003F00ULL;
        emu->reg(sogen::arm64_register::x0, original);
        emu->reg(sogen::arm64_register::x1, 0xA5A5A5A5ULL);

        emu->start(1);
        const auto signed_pointer = emu->reg(sogen::arm64_register::x0);

        emu->start(1);
        const auto authenticated = emu->reg(sogen::arm64_register::x0);

        EXPECT_NE(signed_pointer, original) << "PACIA did not modify the pointer - PAC is a no-op";
        EXPECT_EQ(authenticated, original) << "AUTIA did not recover the original pointer";
    }

    // The round-trip above still passes if PAC is reached by some other route, so pin the two halves of
    // the gate the guest can actually observe: the advertised algorithm, and the SCTLR_EL1 enables that
    // decide whether PACIA translates to anything at all (helper.c:11684-11685). SCR_EL3 and HCR_EL2 are
    // unreadable from EL1 and are verified by the backend itself at construction.
    TEST(UnicornArm64Pac, AdvertisesPauthAndEnablesTheSigningKeys)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD5380620, // mrs x0, id_aa64isar1_el1
                             0xD5381001, // mrs x1, sctlr_el1
                         });

        emu->start(2);

        constexpr uint64_t id_aa64isar1_apa = 0xFULL << 4;
        EXPECT_NE(emu->reg(sogen::arm64_register::x0) & id_aa64isar1_apa, 0ULL) << "UC_CPU_ARM64_MAX stopped advertising ID_AA64ISAR1.APA";

        EXPECT_EQ(emu->reg(sogen::arm64_register::x1) & sctlr_el1_pac_enables, sctlr_el1_pac_enables)
            << "SCTLR_EL1.En{IA,IB,DA,DB} not set - PAC decodes as a no-op";
    }
}

namespace
{
    TEST(UnicornArm64Hooks, ReportsUnmappedMemoryViolation)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD2800540, // mov x0, #42
                         });

        uint64_t violation_address = 0;
        emu->hook_memory_violation(
            [&](sogen::cpu_interface&, const uint64_t address, size_t, sogen::memory_operation, sogen::memory_violation_type) {
                violation_address = address;
                return sogen::memory_violation_continuation::stop;
            });

        emu->reg(sogen::arm64_register::pc, 0xDEAD0000);
        EXPECT_THROW(emu->start(1), std::exception);

        EXPECT_EQ(violation_address, 0xDEAD0000u);
    }

    TEST(UnicornArm64Hooks, ObservesEveryExecutedInstruction)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD503201F, // nop
                             0xD503201F, // nop
                             0xD2800540, // mov x0, #42
                         });

        std::vector<uint64_t> visited;
        emu->hook_memory_execution([&](sogen::cpu_interface&, const uint64_t address) { visited.push_back(address); });

        emu->start(3);

        ASSERT_EQ(visited.size(), 3u);
        EXPECT_EQ(visited[0], code_base);
        EXPECT_EQ(visited[1], code_base + 4);
        EXPECT_EQ(visited[2], code_base + 8);
    }

    TEST(UnicornArm64Hooks, DeleteHookStopsDelivery)
    {
        const auto emu = make_emulator();

        write_code(*emu, {
                             0xD503201F, // nop
                             0xD503201F, // nop
                         });

        size_t count = 0;
        auto* hook = emu->hook_memory_execution([&](sogen::cpu_interface&, uint64_t) { ++count; });
        emu->delete_hook(hook);

        emu->start(2);

        EXPECT_EQ(count, 0u);
    }
}

namespace
{
    TEST(UnicornArm64State, SaveAndRestoreRegistersRoundTrips)
    {
        const auto emu = make_emulator();

        emu->reg(sogen::arm64_register::x0, 0x1111111111111111ULL);
        emu->reg(sogen::arm64_register::x1, 0x2222222222222222ULL);
        const auto saved = emu->save_registers();

        emu->reg(sogen::arm64_register::x0, 0);
        emu->reg(sogen::arm64_register::x1, 0);

        emu->restore_registers(saved);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 0x1111111111111111ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x1), 0x2222222222222222ULL);
    }

    TEST(UnicornArm64State, SaveAndRestoreRevertsFullRegisterFileAndPcAfterExecution)
    {
        const auto emu = make_emulator();

        constexpr std::array<sogen::arm64_register, 8> registers{
            sogen::arm64_register::x0,  sogen::arm64_register::x1, sogen::arm64_register::x2,   sogen::arm64_register::x29,
            sogen::arm64_register::x30, sogen::arm64_register::sp, sogen::arm64_register::nzcv, sogen::arm64_register::d0,
        };

        constexpr std::array<uint64_t, registers.size()> values{
            0x9000000000000000ULL, 0x9000000000000001ULL, 0x9000000000000002ULL,
            0x9000000000000003ULL, 0x9000000000000004ULL, 0x9000000000000005ULL,
            0x20000000ULL,         // NZCV.C - the flag Darwin's BSD syscalls use to signal an error
            0x4048F5C300000000ULL, // an arbitrary D0 bit pattern
        };

        for (size_t i = 0; i < registers.size(); ++i)
        {
            emu->reg(registers[i], values[i]);
        }

        write_code(*emu, {
                             0xD2800540, // mov x0, #42
                             0xD503201F, // nop
                         });

        const auto saved = emu->save_registers();

        const auto expected_pc = emu->reg(sogen::arm64_register::pc);
        std::array<uint64_t, registers.size()> expected{};
        for (size_t i = 0; i < registers.size(); ++i)
        {
            expected[i] = emu->reg(registers[i]);
        }

        emu->start(2);

        ASSERT_NE(emu->reg(sogen::arm64_register::pc), expected_pc) << "sanity check: execution should have moved pc";
        ASSERT_NE(emu->reg(sogen::arm64_register::x0), expected[0]) << "sanity check: execution should have changed x0";

        emu->restore_registers(saved);

        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), expected_pc);
        for (size_t i = 0; i < registers.size(); ++i)
        {
            EXPECT_EQ(emu->reg(registers[i]), expected[i]);
        }
    }

    TEST(UnicornArm64State, RegistersTransplantToFreshEmulatorInstance)
    {
        const auto source = make_emulator();
        source->reg(sogen::arm64_register::x0, 0x1234567890ABCDEFULL);
        source->reg(sogen::arm64_register::x1, 0xFEDCBA0987654321ULL);
        source->reg(sogen::arm64_register::sp, stack_base + 0x40);

        const auto saved = source->save_registers();

        const auto target = make_emulator();
        ASSERT_NE(target->reg(sogen::arm64_register::x0), 0x1234567890ABCDEFULL)
            << "sanity check: fresh emulator should not already carry the source's values";

        target->restore_registers(saved);

        EXPECT_EQ(target->reg(sogen::arm64_register::x0), 0x1234567890ABCDEFULL);
        EXPECT_EQ(target->reg(sogen::arm64_register::x1), 0xFEDCBA0987654321ULL);
        EXPECT_EQ(target->reg(sogen::arm64_register::sp), stack_base + 0x40);
    }

    // Disables pointer authentication mid-run by clearing SCTLR_EL1.En{IA,IB,DA,DB} through a guest
    // "msr sctlr_el1, x2" (EL1 may write its own SCTLR, unlike SCR_EL3/HCR_EL2, so this needs no CP_REG
    // bypass), confirms PACIA has degraded to a no-op, then restores the pre-disable snapshot and
    // confirms PACIA signs again. It establishes that the enabled SCTLR_EL1 the guest can observe comes
    // back through restore_registers(), nothing more; SctlrEl1SurvivesBareContextRoundTrip below pins
    // the register itself.
    //
    // The two PACIA checks use distinct addresses that are each translated exactly once in this test.
    // Reusing one address for both (e.g. writing 0xDAC10020 to it twice) hits an unrelated Unicorn/QEMU
    // quirk where a translated block does not get invalidated on a second identical write, so the second
    // execution silently reuses the first (stale) translation instead of reflecting the current
    // SCTLR_EL1/hflags state - that would make this test pass or fail for the wrong reason.
    TEST(UnicornArm64State, PointerAuthenticationStillFunctionsAfterRegisterRoundTrip)
    {
        const auto emu = make_emulator();

        const auto saved = emu->save_registers();

        write_code(*emu, {
                             0xD5381002, // mrs x2, sctlr_el1
                             0xD2840003, // movz x3, #0x2000
                             0xF2B90003, // movk x3, #0xc800, lsl #16
                             0xCA030042, // eor x2, x2, x3
                             0xD5181002, // msr sctlr_el1, x2
                         });
        emu->start(5);

        constexpr uint64_t original = 0x0000000100003F00ULL;
        constexpr uint32_t pacia_x0_x1 = 0xDAC10020;

        emu->reg(sogen::arm64_register::x0, original);
        emu->reg(sogen::arm64_register::x1, 0xA5A5A5A5ULL);
        emu->write_memory(code_base + 0x100, &pacia_x0_x1, sizeof(pacia_x0_x1));
        emu->reg(sogen::arm64_register::pc, code_base + 0x100);
        emu->start(1);

        ASSERT_EQ(emu->reg(sogen::arm64_register::x0), original)
            << "sanity check: PACIA should have degraded to a no-op after clearing SCTLR_EL1.En{IA,IB,DA,DB}";

        emu->restore_registers(saved);

        emu->reg(sogen::arm64_register::x0, original);
        emu->reg(sogen::arm64_register::x1, 0xA5A5A5A5ULL);
        emu->write_memory(code_base + 0x200, &pacia_x0_x1, sizeof(pacia_x0_x1));
        emu->reg(sogen::arm64_register::pc, code_base + 0x200);
        emu->start(1);

        EXPECT_NE(emu->reg(sogen::arm64_register::x0), original)
            << "PACIA is still a no-op after restore - SCTLR_EL1 pointer authentication enablement did "
               "not survive restore_registers()";
    }

    constexpr uc_arm64_cp_reg sctlr_el1{.crn = 1, .crm = 0, .op0 = 3, .op1 = 0, .op2 = 0, .val = 0};

    uint64_t read_sctlr_el1(uc_engine* uc)
    {
        auto query = sctlr_el1;
        EXPECT_EQ(uc_reg_read(uc, UC_ARM64_REG_CP_REG, &query), UC_ERR_OK);
        return query.val;
    }

    void write_sctlr_el1(uc_engine* uc, const uint64_t value)
    {
        auto update = sctlr_el1;
        update.val = value;
        EXPECT_EQ(uc_reg_write(uc, UC_ARM64_REG_CP_REG, &update), UC_ERR_OK);
    }

    // save_registers()/restore_registers() are uc_context_save()/uc_context_restore() and nothing
    // re-establishes the backend's construction-time pointer-authentication setup afterwards, so PAC
    // survives a restore only for as long as uc_context keeps covering AArch64 system registers. A
    // unicorn bump that dropped them would disable PAC on every restore; this fails loudly if it does.
    //
    // It drives a raw engine because system registers are unreachable through the backend's public
    // register interface: read_raw_register() zero-fills its output buffer before the read, which erases
    // the uc_arm64_cp_reg encoding a CP_REG read has to carry in.
    TEST(UnicornArm64State, SctlrEl1SurvivesBareContextRoundTrip)
    {
        uc_engine* uc{};
        ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
        ASSERT_EQ(uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_MAX), UC_ERR_OK);

        write_sctlr_el1(uc, read_sctlr_el1(uc) | sctlr_el1_pac_enables);
        ASSERT_EQ(read_sctlr_el1(uc) & sctlr_el1_pac_enables, sctlr_el1_pac_enables);

        uc_context* context{};
        ASSERT_EQ(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_CPU), UC_ERR_OK);
        ASSERT_EQ(uc_context_alloc(uc, &context), UC_ERR_OK);
        ASSERT_EQ(uc_context_save(uc, context), UC_ERR_OK);

        write_sctlr_el1(uc, read_sctlr_el1(uc) & ~sctlr_el1_pac_enables);
        ASSERT_EQ(read_sctlr_el1(uc) & sctlr_el1_pac_enables, 0ULL) << "sanity check: the enables should be clear before the restore";

        ASSERT_EQ(uc_context_restore(uc, context), UC_ERR_OK);

        EXPECT_EQ(read_sctlr_el1(uc) & sctlr_el1_pac_enables, sctlr_el1_pac_enables)
            << "uc_context no longer carries SCTLR_EL1 - restore_registers() and deserialize_state() now "
               "silently disable pointer authentication";

        ASSERT_EQ(uc_context_free(context), UC_ERR_OK);
        ASSERT_EQ(uc_close(uc), UC_ERR_OK);
    }

    TEST(UnicornArm64State, SerializeAndDeserializeStateTransplantsAcrossInstances)
    {
        const auto source = make_emulator();

        source->reg(sogen::arm64_register::x0, 0x0F0F0F0F0F0F0F0FULL);
        source->reg(sogen::arm64_register::x1, 0xF0F0F0F0F0F0F0F0ULL);
        source->reg(sogen::arm64_register::sp, stack_base + 0x80);
        source->reg(sogen::arm64_register::pc, code_base + 0x20);

        sogen::utils::buffer_serializer buffer{};
        source->serialize_state(buffer, false);

        const auto target = make_emulator();
        ASSERT_NE(target->reg(sogen::arm64_register::x0), 0x0F0F0F0F0F0F0F0FULL)
            << "sanity check: fresh emulator should not already carry the source's values";

        sogen::utils::buffer_deserializer deserializer{buffer};
        target->deserialize_state(deserializer, false);

        EXPECT_EQ(target->reg(sogen::arm64_register::x0), 0x0F0F0F0F0F0F0F0FULL);
        EXPECT_EQ(target->reg(sogen::arm64_register::x1), 0xF0F0F0F0F0F0F0F0ULL);
        EXPECT_EQ(target->reg(sogen::arm64_register::sp), stack_base + 0x80);
        EXPECT_EQ(target->reg(sogen::arm64_register::pc), code_base + 0x20);
    }
}

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/sysctl.h>
#include <Hypervisor/Hypervisor.h>

namespace
{
    bool host_exposes_hypervisor_framework()
    {
        int supported = 0;
        size_t size = sizeof(supported);
        return ::sysctlbyname("kern.hv_support", &supported, &size, nullptr, 0) == 0 && supported != 0;
    }

    TEST(HypervisorEntitlement, TestRunnerMayCreateAVirtualMachine)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "kern.hv_support is 0: this host does not expose Hypervisor.framework";
        }

        const auto result = hv_vm_create(nullptr);
        ASSERT_EQ(result, HV_SUCCESS) << "hv_vm_create returned 0x" << std::hex << result
                                      << "; 0xfae94007 (HV_DENIED) means this executable is not signed with com.apple.security.hypervisor";

        hv_vm_destroy();
    }
}
#endif

namespace
{
    // The block hook used to report address and size but leave instruction_count at zero, so anything
    // counting instructions per block counted nothing at all. A64 is fixed four-byte width, which is
    // what makes the count exact rather than an estimate.
    TEST(Arm64Backend, BasicBlockHookReportsAnExactInstructionCount)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t base = 0x40000000ull;
        constexpr size_t page = 0x4000;
        emu->map_memory(base, page, sogen::memory_permission::all);

        const std::array<uint32_t, 5> code{
            0xD503201Fu, // nop
            0xD503201Fu, // nop
            0xD503201Fu, // nop
            0xD503201Fu, // nop
            0xD4200000u, // brk #0
        };
        ASSERT_TRUE(emu->try_write_memory(base, code.data(), code.size() * sizeof(uint32_t)));

        std::vector<sogen::basic_block> blocks{};
        emu->hook_basic_block([&](sogen::cpu_interface&, const sogen::basic_block& block) { blocks.push_back(block); });

        emu->reg(sogen::arm64_register::pc, base);

        try
        {
            emu->start(4);
        }
        catch (...)
        {
        }

        ASSERT_FALSE(blocks.empty());
        for (const auto& block : blocks)
        {
            EXPECT_EQ(block.instruction_count, block.size / 4) << "at 0x" << std::hex << block.address;
            EXPECT_NE(block.instruction_count, 0u);
        }
    }
}

namespace
{
    // An emulated kernel has to write pointers the emulated CPU will accept. Signing one host-side and
    // authenticating it inside the guest is the round trip that makes that possible: a plain address
    // does not survive autda -- authenticating one sets the poison bit and the next load faults.
    TEST(Arm64Backend, APointerSignedByTheHostAuthenticatesInsideTheGuest)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;
        constexpr uint64_t discriminator = 0x1234;
        constexpr uint64_t payload = 0xCAFEF00DDEADBEEFull;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);
        ASSERT_TRUE(emu->try_write_memory(data, &payload, sizeof(payload)));

        auto pointer = data;
        ASSERT_TRUE(emu->sign_pointer(pointer, sogen::arm64_pauth_key::data_a, discriminator));
        EXPECT_NE(pointer, data) << "a signed pointer carries a signature above the address";
        EXPECT_EQ(pointer & 0x0000FFFFFFFFFFFFull, data & 0x0000FFFFFFFFFFFFull);

        const std::array<uint32_t, 2> program{
            0xDAC11820u, // autda x0, x1
            0xF9400000u, // ldr   x0, [x0]
        };
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::x0, pointer);
        emu->reg(sogen::arm64_register::x1, discriminator);

        emu->start(2);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), payload) << "the guest authenticated the host's signature and read through it";
    }

    // SMOV reads a signed lane out of a vector register into an X register, and that is what makes the
    // translator emit TCG's ld8s_i64 and ld16s_i64 -- loads from the CPU state holding the vector, not
    // guest memory loads. The interpreter the wasm build runs on had no case for either and aborted the
    // whole module with "TODO tci.c:878" the first time a guest reached one. A JIT build never touches
    // that path, so this only fails where it matters.
    //
    // An ordinary LDRSB does *not* get there: it becomes a guest memory load, which is why the first
    // version of this test passed with the interpreter still unimplemented.
    TEST(Arm64Backend, SignedVectorLaneReadsIntoAnXRegister)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);

        // Both signs on purpose: lane 0 is negative and the halfword at lane 1 is positive, which is what
        // separates sign extension from blanket bit-filling.
        std::array<uint8_t, 16> vector{};
        vector.fill(0);
        vector[0] = 0xFF;
        vector[1] = 0x80;
        vector[2] = 0x34;
        vector[3] = 0x12;
        ASSERT_TRUE(emu->try_write_memory(data, vector.data(), vector.size()));

        const std::array<uint32_t, 5> program{
            0x3DC00020u, // ldr  q0, [x1]
            0x4E012C00u, // smov x0, v0.b[0]
            0x4E032C02u, // smov x2, v0.b[1]
            0x4E022C03u, // smov x3, v0.h[0]
            0x4E062C04u, // smov x4, v0.h[1]
        };
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::x1, data);
        emu->start(program.size());

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 0xFFFFFFFFFFFFFFFFull) << "0xFF sign-extends to all ones";
        EXPECT_EQ(emu->reg(sogen::arm64_register::x2), 0xFFFFFFFFFFFFFF80ull) << "0x80 is negative as a signed byte";
        EXPECT_EQ(emu->reg(sogen::arm64_register::x3), 0xFFFFFFFFFFFF80FFull) << "0x80FF is negative as a halfword";
        EXPECT_EQ(emu->reg(sogen::arm64_register::x4), 0x0000000000001234ull) << "a positive halfword is not filled with ones";
    }

    // Darwin turns pointer authentication off for a process whose main executable is plain arm64 rather
    // than arm64e, which is what lets such a process run against the arm64e shared cache: every aut* in
    // the library code becomes a no-op, so the raw pointers the process's own fixups wrote survive.
    // Measured on 25G76 -- an arm64 build sees a raw isa in its constant CFStrings and in the cache's own
    // class slots, the same source built arm64e sees a signature in both.
    TEST(Arm64Backend, DisablingPointerAuthenticationLetsARawPointerThroughAutda)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;
        constexpr uint64_t discriminator = 0x1234;
        constexpr uint64_t payload = 0xCAFEF00DDEADBEEFull;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);
        ASSERT_TRUE(emu->try_write_memory(data, &payload, sizeof(payload)));

        const std::array<uint32_t, 2> program{
            0xDAC11820u, // autda x0, x1
            0xF9400000u, // ldr   x0, [x0]
        };
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->set_pointer_authentication(false);

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::x0, data);
        emu->reg(sogen::arm64_register::x1, discriminator);
        emu->start(2);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), payload) << "with the keys off autda leaves an unsigned pointer alone";
    }

    // The other half of the same contract: the mode has to be a mode, not a one-way switch, because the
    // signature a signing kernel writes is worthless if the guest stopped checking it.
    TEST(Arm64Backend, ReenablingPointerAuthenticationRestoresEnforcement)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;
        constexpr uint64_t discriminator = 0x1234;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);

        const std::array<uint32_t, 1> program{0xDAC11820u}; // autda x0, x1
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->set_pointer_authentication(false);
        emu->set_pointer_authentication(true);

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::x0, data);
        emu->reg(sogen::arm64_register::x1, discriminator);
        emu->start(1);

        EXPECT_NE(emu->reg(sogen::arm64_register::x0), data) << "with the keys on autda poisons an unsigned pointer";
    }

    // arm64e software keeps live data in the top byte of a pointer: libobjc stores a class's realized
    // flag in bit 63 of class_data_bits_t and branches on it *before* authenticating. That only works
    // because top-byte-ignore confines the signature to bits 54:47, so a signature that reached the tag
    // byte would forge the flag rather than merely look wrong.
    TEST(Arm64Backend, SigningLeavesTheTagByteAloneAndATaggedPointerStillLoads)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;
        constexpr uint64_t discriminator = 0x1234;
        constexpr uint64_t payload = 0xCAFEF00DDEADBEEFull;
        constexpr uint64_t tag = 0x80ull << 56;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);
        ASSERT_TRUE(emu->try_write_memory(data, &payload, sizeof(payload)));

        auto pointer = data | tag;
        ASSERT_TRUE(emu->sign_pointer(pointer, sogen::arm64_pauth_key::data_b, discriminator));
        EXPECT_NE(pointer & ~tag, data) << "the signature still has to land somewhere";
        EXPECT_EQ(pointer >> 56, 0x80ull) << "the signature must not reach into the tag byte";

        const std::array<uint32_t, 2> program{
            0xDAC11C20u, // autdb x0, x1
            0xF9400000u, // ldr   x0, [x0]
        };
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::x0, pointer);
        emu->reg(sogen::arm64_register::x1, discriminator);

        emu->start(2);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), payload) << "a tagged pointer authenticates and loads through its tag";
    }

    // LDRAA/LDRAB authenticate their base register with a modifier of zero, not with SP -- measured on
    // this host: pacdza followed by ldraa loads through the signature while SP holds a stack address.
    // Every arm64e C++ virtual call depends on it, because the vtable pointer in an object is signed
    // with DA and a zero discriminator, and the shared cache's own auth-rebase entries for those slots
    // record diversity 0 and no address diversity to match.
    TEST(Arm64Backend, LoadWithPointerAuthenticationUsesAZeroModifier)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;
        constexpr uint64_t stack = 0x60000000ull;
        constexpr uint64_t payload = 0xCAFEF00DDEADBEEFull;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);
        emu->map_memory(stack, 0x4000, sogen::memory_permission::read_write);
        ASSERT_TRUE(emu->try_write_memory(data + 0x20, &payload, sizeof(payload)));

        auto pointer = data;
        ASSERT_TRUE(emu->sign_pointer(pointer, sogen::arm64_pauth_key::data_a, 0));

        const std::array<uint32_t, 1> program{0xF8204D09u}; // ldraa x9, [x8, #0x20]!
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::sp, stack + 0x2000);
        emu->reg(sogen::arm64_register::x8, pointer);
        emu->start(1);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x9), payload) << "ldraa authenticated the zero-modifier signature";
        EXPECT_EQ(emu->reg(sogen::arm64_register::x8), data + 0x20) << "the writeback carries the authenticated address";
    }

    TEST(Arm64Backend, LoadWithTheBKeyAlsoUsesAZeroModifier)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;
        constexpr uint64_t stack = 0x60000000ull;
        constexpr uint64_t payload = 0x0123456789ABCDEFull;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);
        emu->map_memory(stack, 0x4000, sogen::memory_permission::read_write);
        ASSERT_TRUE(emu->try_write_memory(data + 0x20, &payload, sizeof(payload)));

        auto pointer = data;
        ASSERT_TRUE(emu->sign_pointer(pointer, sogen::arm64_pauth_key::data_b, 0));

        const std::array<uint32_t, 1> program{0xF8A04D09u}; // ldrab x9, [x8, #0x20]!
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::sp, stack + 0x2000);
        emu->reg(sogen::arm64_register::x8, pointer);
        emu->start(1);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x9), payload) << "ldrab authenticated the zero-modifier signature";
    }

    TEST(Arm64Backend, AnUnsignedPointerDoesNotSurviveAuthentication)
    {
        const auto emu = sogen::unicorn::create_arm64_emulator();
        ASSERT_NE(emu, nullptr);

        constexpr uint64_t code = 0x40000000ull;
        constexpr uint64_t data = 0x50000000ull;

        emu->map_memory(code, 0x4000, sogen::memory_permission::all);
        emu->map_memory(data, 0x4000, sogen::memory_permission::read_write);

        const std::array<uint32_t, 1> program{0xDAC11820u}; // autda x0, x1
        ASSERT_TRUE(emu->try_write_memory(code, program.data(), program.size() * sizeof(uint32_t)));

        emu->reg(sogen::arm64_register::pc, code);
        emu->reg(sogen::arm64_register::x0, data);
        emu->reg(sogen::arm64_register::x1, uint64_t{0x1234});

        emu->start(1);

        // This is why the slide info cannot simply be rewritten with plain addresses.
        EXPECT_NE(emu->reg(sogen::arm64_register::x0), data) << "authenticating an unsigned pointer has to poison it";
    }
}

namespace
{
    TEST(Arm64BackendSelection, FactoryReturnsAMappableEmulator)
    {
        static_assert(std::is_base_of_v<sogen::arm64_64_emulator, sogen::arm64_mappable_emulator>);
        static_assert(std::is_same_v<decltype(sogen::create_arm64_emulator()), std::unique_ptr<sogen::arm64_mappable_emulator>>);

        const auto emu = sogen::create_arm64_emulator();
        emu->map_memory(0x10000, 0x1000, sogen::memory_permission::all);

        constexpr uint32_t value = 0xD503201F;
        emu->write_memory(0x10000, &value, sizeof(value));
        EXPECT_EQ(emu->read_memory<uint32_t>(0x10000), value);
    }
}

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    TEST(HvfArm64, IsSelectableAndReportsItsName)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "kern.hv_support is 0: this host does not expose Hypervisor.framework";
        }

        const auto emu = sogen::create_arm64_emulator(sogen::backend_type::hvf);
        ASSERT_NE(emu, nullptr);
        EXPECT_EQ(emu->get_name(), "Hypervisor.framework");
    }

    TEST(HvfArm64, TwoInstancesShareTheProcessVirtualMachine)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "kern.hv_support is 0: this host does not expose Hypervisor.framework";
        }

        const auto first = sogen::create_arm64_emulator(sogen::backend_type::hvf);
        const auto second = sogen::create_arm64_emulator(sogen::backend_type::hvf);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
    }

    TEST(HvfArm64, EnvironmentSelectsTheBackend)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "kern.hv_support is 0: this host does not expose Hypervisor.framework";
        }

        ::setenv("EMULATOR_HVF", "1", 1);
        const auto emu = sogen::create_arm64_emulator_from_environment();
        ::unsetenv("EMULATOR_HVF");

        EXPECT_EQ(emu->get_name(), "Hypervisor.framework");
    }
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    std::unique_ptr<sogen::arm64_mappable_emulator> make_hvf_emulator()
    {
        return sogen::create_arm64_emulator(sogen::backend_type::hvf);
    }

    TEST(HvfArm64Registers, ReadsAndWritesGeneralPurposeRegisters)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        emu->reg(sogen::arm64_register::x0, 0x1122334455667788ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 0x1122334455667788ULL);

        emu->reg(sogen::arm64_register::x29, 0xFEEDFACEULL);
        emu->reg(sogen::arm64_register::x30, 0xDEADBEEFULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x30), 0xDEADBEEFULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x29), 0xFEEDFACEULL);

        // x29 and x30 sit *before* x0 in the enum (ids 1 and 2 against x0's 199), so a classifier that
        // folds them in with the others aliases them onto real registers. Re-reading x0 afterwards is
        // what catches that; checking each register only against the value just written cannot, because
        // a wrong-but-consistent mapping passes.
        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 0x1122334455667788ULL);

        emu->reg(sogen::arm64_register::pc, 0x0000000100000000ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), 0x0000000100000000ULL);

        emu->reg(sogen::arm64_register::sp, 0x00007FF800000000ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::sp), 0x00007FF800000000ULL);

        emu->reg(sogen::arm64_register::nzcv, 0x20000000ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::nzcv), 0x20000000ULL);
    }

    TEST(HvfArm64Registers, NzcvIsProjectedOutOfCpsrRatherThanBeingIt)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        // nzcv is the condition-flag view of CPSR[31:28] only. Writing every bit and reading back just
        // the flags is what pins that, and it does not depend on what CPSR happens to hold at creation
        // (measured as 0 on this OS). Darwin signals syscall failure in NZCV.C, so a reader that
        // returned the whole register would see every call as failed.
        emu->reg(sogen::arm64_register::nzcv, 0xFFFFFFFFULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::nzcv), 0xF0000000ULL);

        emu->reg(sogen::arm64_register::nzcv, 0x20000000ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::nzcv), 0x20000000ULL);
    }

    TEST(HvfArm64Registers, ReadsAndWritesThreadPointer)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        emu->set_thread_pointer(0x7000000000ULL);
        EXPECT_EQ(emu->get_thread_pointer(), 0x7000000000ULL);
    }

    TEST(HvfArm64Registers, AreReachableFromANonOwningThread)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        emu->reg(sogen::arm64_register::x5, 0xA5A5A5A5A5A5A5A5ULL);

        uint64_t observed = 0;
        std::thread reader{[&] { observed = emu->reg(sogen::arm64_register::x5); }};
        reader.join();

        EXPECT_EQ(observed, 0xA5A5A5A5A5A5A5A5ULL);
    }

    TEST(HvfArm64Registers, TwoInstancesOnOneThreadHoldIndependentState)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto first = make_hvf_emulator();
        const auto second = make_hvf_emulator();

        first->reg(sogen::arm64_register::x0, 0x1111111111111111ULL);
        second->reg(sogen::arm64_register::x0, 0x2222222222222222ULL);

        EXPECT_EQ(first->reg(sogen::arm64_register::x0), 0x1111111111111111ULL);
        EXPECT_EQ(second->reg(sogen::arm64_register::x0), 0x2222222222222222ULL);
    }
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    TEST(HvfArm64Memory, ReadsAndWritesGuestMemory)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();
        emu->map_memory(0x10000, 0x10000, sogen::memory_permission::all);

        constexpr uint32_t value = 0xCAFEBABE;
        emu->write_memory(0x10000, &value, sizeof(value));
        EXPECT_EQ(emu->read_memory<uint32_t>(0x10000), value);
    }

    TEST(HvfArm64Memory, RejectsAccessToUnmappedGuestAddresses)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        uint32_t sink{};
        EXPECT_FALSE(emu->try_read_memory(0xDEAD0000, &sink, sizeof(sink)));
        EXPECT_FALSE(emu->try_write_memory(0xDEAD0000, &sink, sizeof(sink)));
    }

    TEST(HvfArm64Memory, MapsAdjacentFourKilobytePagesWithDifferentPermissions)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        emu->map_memory(0x40000, 0x2000, sogen::memory_permission::read_write);
        emu->apply_memory_protection(0x41000, 0x1000, sogen::memory_permission::read);

        constexpr uint64_t value = 0x1234567890ABCDEFULL;
        emu->write_memory(0x40000, &value, sizeof(value));
        EXPECT_EQ(emu->read_memory<uint64_t>(0x40000), value);
        EXPECT_EQ(emu->read_memory<uint64_t>(0x41000), 0ULL);
    }

    TEST(HvfArm64Memory, UnmapsASubRangeWithoutDisturbingItsNeighbour)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        emu->map_memory(0x50000, 0x2000, sogen::memory_permission::read_write);
        constexpr uint32_t value = 0x11223344;
        emu->write_memory(0x50000, &value, sizeof(value));

        emu->unmap_memory(0x51000, 0x1000);

        EXPECT_EQ(emu->read_memory<uint32_t>(0x50000), value);
        uint32_t sink{};
        EXPECT_FALSE(emu->try_read_memory(0x51000, &sink, sizeof(sink)));
    }

    // hv_vm_map demands 16 KiB alignment on the host VA and the size alike. This is the only place that
    // granularity reaches a caller, and a bare HV_BAD_ARGUMENT from inside hv_vm_map would not say which
    // operand was wrong.
    TEST(HvfArm64Memory, HostAliasingRefusesAMisalignedPointerOrSize)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        auto* host = static_cast<uint8_t*>(::mmap(nullptr, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        ASSERT_NE(host, MAP_FAILED);

        // hv_vm_map rejects both of these on its own, so throwing is not what is being pinned here --
        // the message is. Its whole value is naming the operand, which "hv_vm_map failed: 0x..." does
        // not do.
        const auto message_of = [&](void* pointer, const size_t size) {
            try
            {
                emu->map_host_memory(0x70000, size, pointer, sogen::memory_permission::read_write);
            }
            catch (const std::runtime_error& error)
            {
                return std::string{error.what()};
            }

            return std::string{"<no exception>"};
        };

        EXPECT_NE(message_of(host + 0x1000, 0x4000).find("16 KiB-aligned"), std::string::npos);
        EXPECT_NE(message_of(host, 0x1000).find("16 KiB-aligned"), std::string::npos);

        ::munmap(host, 0x8000);
    }

    TEST(HvfArm64Memory, AliasesHostMemoryCoherently)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();
        EXPECT_TRUE(emu->host_memory_aliasing_is_coherent());

        void* host = ::mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        ASSERT_NE(host, MAP_FAILED);

        emu->map_host_memory(0x60000, 0x4000, host, sogen::memory_permission::read_write);

        constexpr uint32_t value = 0x55667788;
        emu->write_memory(0x60000, &value, sizeof(value));

        uint32_t observed{};
        std::memcpy(&observed, host, sizeof(observed));
        EXPECT_EQ(observed, value);

        ::munmap(host, 0x4000);
    }
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    constexpr uint64_t hvf_code_base = 0x0000000100000000ULL;
    constexpr uint64_t hvf_stack_base = 0x00007FF800000000ULL;

    std::unique_ptr<sogen::arm64_mappable_emulator> make_running_hvf_emulator()
    {
        auto emu = make_hvf_emulator();
        emu->map_memory(hvf_code_base, 0x10000, sogen::memory_permission::all);
        emu->map_memory(hvf_stack_base, 0x10000, sogen::memory_permission::read_write);
        emu->reg(sogen::arm64_register::sp, hvf_stack_base + 0x10000 - 0x100);
        return emu;
    }

    void write_hvf_code(sogen::arm64_64_emulator& emu, const std::vector<uint32_t>& words)
    {
        emu.write_memory(hvf_code_base, words.data(), words.size() * sizeof(uint32_t));
        emu.reg(sogen::arm64_register::pc, hvf_code_base);
    }

    TEST(HvfArm64Execution, ExecutesMovAndStopsAfterInstructionCount)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        write_hvf_code(*emu, {
                                 0xD2800540, // mov x0, #42
                                 0xD503201F, // nop
                             });

        emu->start(2);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 42u);
        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), hvf_code_base + 8);
    }

    TEST(HvfArm64Execution, StopsFromAnotherThread)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();
        EXPECT_TRUE(emu->is_stop_thread_safe());

        // The spin deliberately sits at a different address from the entry point. A cancel is taken
        // straight to EL2, so ELR_EL1 still holds whatever was written before entry -- if the pc came
        // from there instead of the real one, this reports the entry address and looks correct.
        write_hvf_code(*emu, {
                                 0x14000002, // b .+8
                                 0xD503201F, // nop
                                 0x14000000, // b .
                             });

        std::thread stopper{[&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            emu->stop();
        }};

        emu->start();
        stopper.join();

        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), hvf_code_base + 8);
    }
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    TEST(HvfArm64Hooks, InvokesInstructionHookOnSvc)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        write_hvf_code(*emu, {
                                 0xD2800090, // mov x16, #4
                                 0xD4001001, // svc #0x80
                                 0xD503201F, // nop
                             });

        uint64_t observed_syscall_number = 0;
        uint64_t observed_immediate = 0;
        size_t hook_count = 0;

        emu->hook_instruction(sogen::arm64_hookable_instructions::svc, [&](sogen::cpu_interface&, const uint64_t data) {
            ++hook_count;
            observed_immediate = data;
            observed_syscall_number = emu->reg(sogen::arm64_register::x16);
            return sogen::instruction_hook_continuation::run_instruction;
        });

        emu->start(3);

        EXPECT_EQ(hook_count, 1u);
        EXPECT_EQ(observed_syscall_number, 4u);
        EXPECT_EQ(observed_immediate, 0x80u);
    }

    TEST(HvfArm64Hooks, InstructionHookSeesTheFaultingProgramCounter)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        write_hvf_code(*emu, {
                                 0xD4001001, // svc #0x80
                             });

        uint64_t observed_pc = 0;
        emu->hook_instruction(sogen::arm64_hookable_instructions::svc, [&](sogen::cpu_interface& cpu, uint64_t) {
            observed_pc = static_cast<sogen::arm64_64_cpu&>(cpu).read_instruction_pointer();
            return sogen::instruction_hook_continuation::skip_instruction;
        });

        emu->start(1);

        EXPECT_EQ(observed_pc, hvf_code_base);
        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), hvf_code_base + 4);
    }

    TEST(HvfArm64Hooks, InvokesInstructionHookOnBrk)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        write_hvf_code(*emu, {
                                 0xD4200000, // brk #0
                             });

        size_t hook_count = 0;
        emu->hook_instruction(sogen::arm64_hookable_instructions::brk, [&](sogen::cpu_interface&, uint64_t) {
            ++hook_count;
            return sogen::instruction_hook_continuation::skip_instruction;
        });

        emu->start(1);

        EXPECT_EQ(hook_count, 1u);
    }

    TEST(HvfArm64Hooks, ReportsAProtectionViolationAtTheExactAddress)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();
        emu->map_memory(0x70000, 0x1000, sogen::memory_permission::read);

        write_hvf_code(*emu, {
                                 0xF9000020, // str x0, [x1]
                             });
        emu->reg(sogen::arm64_register::x1, 0x70040);

        uint64_t violation_address = 0;
        sogen::memory_operation violation_operation{};
        sogen::memory_violation_type violation_type{};

        emu->hook_memory_violation([&](sogen::cpu_interface&, const uint64_t address, size_t, const sogen::memory_operation operation,
                                       const sogen::memory_violation_type type) {
            violation_address = address;
            violation_operation = operation;
            violation_type = type;
            return sogen::memory_violation_continuation::stop;
        });

        emu->start(1);

        EXPECT_EQ(violation_address, 0x70040u);
        EXPECT_EQ(violation_operation, sogen::memory_operation::write);
        EXPECT_EQ(violation_type, sogen::memory_violation_type::protection);
    }

    TEST(HvfArm64Hooks, RestartResumesTheFaultingInstruction)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();
        emu->map_memory(0x80000, 0x1000, sogen::memory_permission::read);

        write_hvf_code(*emu, {
                                 0xF9000020, // str x0, [x1]
                                 0xD4001001, // svc #0x80
                             });
        emu->reg(sogen::arm64_register::x0, 0xCAFEBABEDEADBEEFULL);
        emu->reg(sogen::arm64_register::x1, 0x80040);

        emu->hook_memory_violation([&](sogen::cpu_interface&, uint64_t, size_t, sogen::memory_operation, sogen::memory_violation_type) {
            emu->apply_memory_protection(0x80000, 0x1000, sogen::memory_permission::read_write);
            return sogen::memory_violation_continuation::restart;
        });

        size_t syscalls = 0;
        emu->hook_instruction(sogen::arm64_hookable_instructions::svc, [&](sogen::cpu_interface&, uint64_t) {
            ++syscalls;
            return sogen::instruction_hook_continuation::skip_instruction;
        });

        emu->start(2);

        EXPECT_EQ(syscalls, 1u);
        EXPECT_EQ(emu->read_memory<uint64_t>(0x80040), 0xCAFEBABEDEADBEEFULL);
    }
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    TEST(HvfArm64Pac, PaciaThenAutiaRoundTripsWithAppleKeys)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        write_hvf_code(*emu, {
                                 0xDAC10020, // pacia x0, x1
                                 0xDAC11020, // autia x0, x1
                             });

        constexpr uint64_t original = 0x0000000100003F00ULL;
        emu->reg(sogen::arm64_register::x0, original);
        emu->reg(sogen::arm64_register::x1, 0xA5A5A5A5ULL);

        emu->start(1);
        const auto signed_pointer = emu->reg(sogen::arm64_register::x0);

        emu->start(1);
        const auto authenticated = emu->reg(sogen::arm64_register::x0);

        EXPECT_NE(signed_pointer, original) << "PACIA did not modify the pointer - SCTLR_EL1.EnIA is clear";
        EXPECT_EQ(authenticated, original) << "AUTIA did not recover the original pointer";
    }

    TEST(HvfArm64State, SaveAndRestoreRegistersRoundTrips)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        emu->reg(sogen::arm64_register::x0, 0x1111111111111111ULL);
        emu->reg(sogen::arm64_register::x1, 0x2222222222222222ULL);
        const auto saved = emu->save_registers();

        emu->reg(sogen::arm64_register::x0, 0);
        emu->reg(sogen::arm64_register::x1, 0);
        emu->restore_registers(saved);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 0x1111111111111111ULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x1), 0x2222222222222222ULL);
    }

    // A restored snapshot whose PAC keys were not captured signs differently, so every authentication
    // the guest performed before the snapshot fails afterwards.
    TEST(HvfArm64State, PointerAuthenticationKeysSurviveARegisterRoundTrip)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        write_hvf_code(*emu, {
                                 0xDAC10020, // pacia x0, x1
                             });

        constexpr uint64_t original = 0x0000000100003F00ULL;
        emu->reg(sogen::arm64_register::x0, original);
        emu->reg(sogen::arm64_register::x1, 0xA5A5A5A5ULL);

        const auto saved = emu->save_registers();
        emu->start(1);
        const auto before = emu->reg(sogen::arm64_register::x0);

        emu->restore_registers(saved);
        emu->start(1);
        const auto after = emu->reg(sogen::arm64_register::x0);

        EXPECT_NE(before, original) << "sanity check: PACIA should have signed the pointer";
        EXPECT_EQ(after, before) << "the PAC keys were not part of the saved register state";
    }

    TEST(HvfArm64State, RestoreRejectsASnapshotOfTheWrongSize)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        auto saved = emu->save_registers();
        ASSERT_FALSE(saved.empty());
        saved.pop_back();

        EXPECT_THROW(emu->restore_registers(saved), std::runtime_error);
    }

    // The version tag exists so a snapshot written by an older layout fails loudly instead of being
    // reinterpreted field by field as the current one.
    TEST(HvfArm64State, DeserializeRejectsAnUnknownStateVersion)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        sogen::utils::buffer_serializer buffer{};
        buffer.write(static_cast<uint32_t>(0xDEADBEEF));
        buffer.write(emu->save_registers());

        // Reading a garbage payload throws on its own, so the throw alone proves nothing. The message
        // is what says the version was checked rather than the buffer simply running out.
        sogen::utils::buffer_deserializer deserializer{buffer};

        std::string message{};
        try
        {
            emu->deserialize_state(deserializer, false);
        }
        catch (const std::runtime_error& error)
        {
            message = error.what();
        }

        EXPECT_NE(message.find("state version"), std::string::npos) << "actual message: " << message;
    }

    TEST(HvfArm64State, SerializeAndDeserializeStateTransplantsAcrossInstances)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto source = make_running_hvf_emulator();

        source->reg(sogen::arm64_register::x0, 0x0F0F0F0F0F0F0F0FULL);
        source->reg(sogen::arm64_register::pc, hvf_code_base + 0x20);

        sogen::utils::buffer_serializer buffer{};
        source->serialize_state(buffer, false);

        const auto target = make_running_hvf_emulator();
        sogen::utils::buffer_deserializer deserializer{buffer};
        target->deserialize_state(deserializer, false);

        EXPECT_EQ(target->reg(sogen::arm64_register::x0), 0x0F0F0F0F0F0F0F0FULL);
        EXPECT_EQ(target->reg(sogen::arm64_register::pc), hvf_code_base + 0x20);
    }
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    TEST(HvfArm64Mmio, ServicesReadsFromTheReadCallback)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        uint64_t backing = 0x0123456789ABCDEFULL;
        emu->map_mmio(
            0x90000, 0x1000, [&](uint64_t, void* data, const size_t size) { std::memcpy(data, &backing, std::min(size, sizeof(backing))); },
            [&](uint64_t, const void* data, const size_t size) { std::memcpy(&backing, data, std::min(size, sizeof(backing))); });

        // x5 rather than x0: SRT is decoded out of the syndrome, and a field read from the wrong bit
        // offset still yields 0 for x0, so an x0 destination cannot tell a correct decode from a wrong
        // one.
        write_hvf_code(*emu, {
                                 0xF9400025, // ldr x5, [x1]
                             });
        emu->reg(sogen::arm64_register::x1, 0x90000);

        emu->start(1);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x5), 0x0123456789ABCDEFULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), hvf_code_base + 4) << "a serviced access is complete, not retried";
    }

    TEST(HvfArm64Mmio, RoutesWritesToTheWriteCallback)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        uint64_t backing = 0;
        uint64_t observed = 0;
        emu->map_mmio(
            0x90000, 0x1000, [&](uint64_t, void* data, const size_t size) { std::memcpy(data, &backing, std::min(size, sizeof(backing))); },
            [&](uint64_t, const void* data, const size_t size) { std::memcpy(&observed, data, std::min(size, sizeof(observed))); });

        write_hvf_code(*emu, {
                                 0xF9000027, // str x7, [x1]
                             });
        emu->reg(sogen::arm64_register::x7, 0xFEEDFACECAFEBEEFULL);
        emu->reg(sogen::arm64_register::x1, 0x90000);

        emu->start(1);

        EXPECT_EQ(observed, 0xFEEDFACECAFEBEEFULL);
        EXPECT_EQ(emu->reg(sogen::arm64_register::pc), hvf_code_base + 4) << "a serviced access is complete, not retried";
    }

    // SRT == 31 is the zero register, not x31: a load discards its value and a store writes zero.
    // Indexing a 31-entry file with 31 is the obvious bug here.
    TEST(HvfArm64Mmio, TreatsTheZeroRegisterAsZeroRatherThanAThirtySecondRegister)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        uint64_t observed = 0xFFFFFFFFFFFFFFFFULL;
        emu->map_mmio(
            0x90000, 0x1000, [](uint64_t, void* data, const size_t size) { std::memset(data, 0, size); },
            [&](uint64_t, const void* data, const size_t size) { std::memcpy(&observed, data, std::min(size, sizeof(observed))); });

        write_hvf_code(*emu, {
                                 0xF900003F, // str xzr, [x1]
                             });
        emu->reg(sogen::arm64_register::x1, 0x90000);

        // Every general register carries a sentinel, so folding SRT 31 onto any of them writes that
        // sentinel instead of zero. Leaving them at their defaults would make x0 read as zero anyway.
        for (int reg = static_cast<int>(sogen::arm64_register::x0); reg <= static_cast<int>(sogen::arm64_register::x28); ++reg)
        {
            emu->reg(static_cast<sogen::arm64_register>(reg), 0xBADC0FFEE0DDF00DULL);
        }
        emu->reg(sogen::arm64_register::x1, 0x90000);

        emu->start(1);

        EXPECT_EQ(observed, 0ULL);
    }
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
namespace
{
    TEST(HvfArm64Capabilities, ReportsWhatItCannotInstrument)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_hvf_emulator();

        EXPECT_FALSE(emu->supports_global_memory_execution_hooks());
        EXPECT_FALSE(emu->supports_instruction_counting());
        EXPECT_TRUE(emu->is_stop_thread_safe());
        EXPECT_NO_THROW(emu->set_memory_execution_hook_mode(sogen::arm64_mappable_emulator::memory_execution_hook_mode::int3));
    }

    TEST(HvfArm64Capabilities, PatchesAnAddressSpecificExecutionHook)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        // The hooked instruction runs twice. Once is not enough: a backend that restores the displaced
        // word and never puts the brk back reports the first hit and then goes quiet, which a
        // single-visit program cannot tell from correct behaviour.
        write_hvf_code(*emu, {
                                 0xD2800001, // mov x1, #0
                                 0x91000421, // add x1, x1, #1   <- hooked
                                 0xF100083F, // cmp x1, #2
                                 0x54FFFFC1, // b.ne -8
                                 0xD2800540, // mov x0, #42
                                 0xD4001001, // svc #0x80
                             });

        std::vector<uint64_t> visited;
        emu->hook_memory_execution(hvf_code_base + 4, [&](sogen::cpu_interface&, const uint64_t address) { visited.push_back(address); });

        // The svc terminates the run because the handler says so. Nothing about a syscall ends an
        // unbounded run on its own -- on every backend that is the handler's job, which is why the macOS
        // exit handler calls stop() explicitly.
        emu->hook_instruction(sogen::arm64_hookable_instructions::svc, [&](sogen::cpu_interface&, uint64_t) {
            emu->stop();
            return sogen::instruction_hook_continuation::skip_instruction;
        });

        emu->start();

        ASSERT_EQ(visited.size(), 2u) << "the brk was not put back after the displaced instruction ran";
        EXPECT_EQ(visited[0], hvf_code_base + 4);
        EXPECT_EQ(visited[1], hvf_code_base + 4);
        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), 42u) << "the patched BRK was not restored before the instruction was re-executed";
    }

    TEST(HvfArm64Capabilities, ReadsASystemRegisterByItsArchitecturalEncoding)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        // ID_AA64ISAR1_EL1 is (3,0,0,6,1). Its API field is non-zero on this silicon, which is what
        // makes the guest's pointer authentication real rather than emulated.
        const auto isar1 = emu->read_system_register(3, 0, 0, 6, 1);
        EXPECT_NE((isar1 >> 8) & 0xF, 0u) << "ID_AA64ISAR1_EL1.API is zero: read the wrong register";

        // SCTLR_EL1 is (3,0,1,0,0), whose crn is 1 rather than 0. A crn shifted by the wrong amount
        // still lands on the same register when crn is zero, so ISAR1 alone cannot catch it.
        const auto sctlr = emu->read_system_register(3, 0, 1, 0, 0);
        EXPECT_NE(sctlr & 1u, 0u) << "SCTLR_EL1.M is clear, but the backend enabled the MMU";
    }

    // There is no host-side signing primitive under HVF -- the keys live in the vCPU -- so the emulator
    // signs by executing the instruction in the guest. The signature must therefore be the one the guest
    // itself produces.
    TEST(HvfArm64Capabilities, HostSignedPointersAuthenticateInsideTheGuest)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();

        constexpr uint64_t original = 0x0000000100003F00ULL;
        constexpr uint64_t discriminator = 0x5A5A5A5AULL;

        auto pointer = original;
        ASSERT_TRUE(emu->sign_pointer(pointer, sogen::arm64_pauth_key::instruction_a, discriminator));
        EXPECT_NE(pointer, original) << "the pointer was not signed";

        write_hvf_code(*emu, {
                                 0xDAC11020, // autia x0, x1
                             });
        emu->reg(sogen::arm64_register::x0, pointer);
        emu->reg(sogen::arm64_register::x1, discriminator);

        emu->start(1);

        EXPECT_EQ(emu->reg(sogen::arm64_register::x0), original) << "the guest could not authenticate the host's signature";
    }

    TEST(HvfArm64Capabilities, RejectsMoreWatchpointsThanTheHardwareHas)
    {
        if (!host_exposes_hypervisor_framework())
        {
            GTEST_SKIP() << "no Hypervisor.framework";
        }
        const auto emu = make_running_hvf_emulator();
        emu->map_memory(0xA0000, 0x1000, sogen::memory_permission::read_write);

        const auto noop = [](sogen::cpu_interface&, uint64_t, const void*, size_t) {};
        for (uint64_t i = 0; i < 4; ++i)
        {
            EXPECT_NO_THROW((void)emu->hook_memory_write(0xA0000 + i * 8, 8, noop));
        }

        // The message, not just the throw: without the limit check the fifth hook indexes the
        // watchpoint register table out of bounds and throws from there instead, which looks identical
        // to a caller and is a far worse failure.
        std::string message{};
        try
        {
            (void)emu->hook_memory_write(0xA0100, 8, noop);
        }
        catch (const std::runtime_error& error)
        {
            message = error.what();
        }

        EXPECT_NE(message.find("watchpoints are already in use"), std::string::npos) << "actual message: " << message;
    }
}
#endif
