#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <arch_emulator.hpp>
#include <arm64_register.hpp>
#include <backend_selection.hpp>

namespace
{
    struct backend_parameter
    {
        sogen::backend_type backend;
        const char* name;
    };

    constexpr uint64_t code_base = 0x0000000100000000ULL;
    constexpr uint64_t stack_base = 0x00007FF800000000ULL;
    constexpr size_t region_size = 0x10000;

    // The point of this suite is that the backend-neutral behaviour is written once and run against
    // every engine. Where an engine genuinely cannot do something, it says so through the interface and
    // the test skips -- never through a hardcoded list of backend names.
    class Arm64Conformance : public testing::TestWithParam<backend_parameter>
    {
      protected:
        void SetUp() override
        {
            try
            {
                this->emu_ = sogen::create_arm64_emulator(GetParam().backend);
            }
            catch (const std::exception& e)
            {
                GTEST_SKIP() << GetParam().name << " backend unavailable: " << e.what();
            }

            this->emu_->map_memory(code_base, region_size, sogen::memory_permission::all);
            this->emu_->map_memory(stack_base, region_size, sogen::memory_permission::read_write);
            this->emu_->reg(sogen::arm64_register::sp, stack_base + region_size - 0x100);
        }

        void write_code(const std::vector<uint32_t>& words) const
        {
            this->emu_->write_memory(code_base, words.data(), words.size() * sizeof(uint32_t));
            this->emu_->reg(sogen::arm64_register::pc, code_base);
        }

        std::unique_ptr<sogen::arm64_mappable_emulator> emu_{};
    };

    TEST_P(Arm64Conformance, ReadsAndWritesGeneralPurposeRegisters)
    {
        this->emu_->reg(sogen::arm64_register::x0, 0x1122334455667788ULL);
        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::x0), 0x1122334455667788ULL);

        this->emu_->reg(sogen::arm64_register::x29, 0xFEEDFACEULL);
        this->emu_->reg(sogen::arm64_register::x30, 0xDEADBEEFULL);
        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::x30), 0xDEADBEEFULL);
        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::x29), 0xFEEDFACEULL);

        // x29 and x30 precede x0 in the register enum, so an engine that folds them in with the rest
        // aliases them onto real registers. Re-reading x0 is what catches that.
        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::x0), 0x1122334455667788ULL);
    }

    TEST_P(Arm64Conformance, ReadsAndWritesThreadPointer)
    {
        this->emu_->set_thread_pointer(0x7000000000ULL);
        EXPECT_EQ(this->emu_->get_thread_pointer(), 0x7000000000ULL);
    }

    TEST_P(Arm64Conformance, ReadsAndWritesGuestMemory)
    {
        constexpr uint32_t value = 0xCAFEBABE;
        this->emu_->write_memory(code_base, &value, sizeof(value));
        EXPECT_EQ(this->emu_->read_memory<uint32_t>(code_base), value);
    }

    TEST_P(Arm64Conformance, ExecutesMovAndStopsAfterInstructionCount)
    {
        this->write_code({
            0xD2800540, // mov x0, #42
            0xD503201F, // nop
        });

        this->emu_->start(2);

        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::x0), 42u);
        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::pc), code_base + 8);
    }

    TEST_P(Arm64Conformance, InvokesInstructionHookOnSvc)
    {
        this->write_code({
            0xD2800090, // mov x16, #4
            0xD4001001, // svc #0x80
            0xD503201F, // nop
        });

        uint64_t observed = 0;
        size_t hook_count = 0;

        this->emu_->hook_instruction(sogen::arm64_hookable_instructions::svc, [&](sogen::cpu_interface&, uint64_t) {
            ++hook_count;
            observed = this->emu_->reg(sogen::arm64_register::x16);
            return sogen::instruction_hook_continuation::run_instruction;
        });

        this->emu_->start(3);

        EXPECT_EQ(hook_count, 1u);
        EXPECT_EQ(observed, 4u);
    }

    TEST_P(Arm64Conformance, PaciaThenAutiaRoundTrips)
    {
        this->write_code({
            0xDAC10020, // pacia x0, x1
            0xDAC11020, // autia x0, x1
        });

        constexpr uint64_t original = 0x0000000100003F00ULL;
        this->emu_->reg(sogen::arm64_register::x0, original);
        this->emu_->reg(sogen::arm64_register::x1, 0xA5A5A5A5ULL);

        this->emu_->start(1);
        const auto signed_pointer = this->emu_->reg(sogen::arm64_register::x0);

        this->emu_->start(1);
        const auto authenticated = this->emu_->reg(sogen::arm64_register::x0);

        EXPECT_NE(signed_pointer, original) << "PACIA did not modify the pointer";
        EXPECT_EQ(authenticated, original) << "AUTIA did not recover the original pointer";
    }

    TEST_P(Arm64Conformance, SaveAndRestoreRegistersRoundTrips)
    {
        this->emu_->reg(sogen::arm64_register::x0, 0x1111111111111111ULL);
        this->emu_->reg(sogen::arm64_register::x1, 0x2222222222222222ULL);
        const auto saved = this->emu_->save_registers();

        this->emu_->reg(sogen::arm64_register::x0, 0);
        this->emu_->reg(sogen::arm64_register::x1, 0);
        this->emu_->restore_registers(saved);

        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::x0), 0x1111111111111111ULL);
        EXPECT_EQ(this->emu_->reg(sogen::arm64_register::x1), 0x2222222222222222ULL);
    }

    TEST_P(Arm64Conformance, ObservesEveryExecutedInstruction)
    {
        if (!this->emu_->supports_global_memory_execution_hooks())
        {
            GTEST_SKIP() << GetParam().name << " runs guest code natively and has no per-instruction hook point";
        }

        this->write_code({
            0xD503201F, // nop
            0xD503201F, // nop
            0xD2800540, // mov x0, #42
        });

        std::vector<uint64_t> visited;
        this->emu_->hook_memory_execution([&](sogen::cpu_interface&, const uint64_t address) { visited.push_back(address); });

        this->emu_->start(3);

        ASSERT_EQ(visited.size(), 3u);
        EXPECT_EQ(visited[0], code_base);
        EXPECT_EQ(visited[1], code_base + 4);
        EXPECT_EQ(visited[2], code_base + 8);
    }

    INSTANTIATE_TEST_SUITE_P(Backends, Arm64Conformance,
                             testing::Values(backend_parameter{sogen::backend_type::unicorn, "unicorn"},
                                             backend_parameter{sogen::backend_type::hvf, "hvf"}),
                             [](const testing::TestParamInfo<backend_parameter>& info) { return info.param.name; });
}
