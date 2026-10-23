#define UNICORN_EMULATOR_IMPL
#include "unicorn_arm64_emulator.hpp"
#include "unicorn_emulator.hpp"

#include <sstream>

namespace sogen::unicorn
{
    namespace
    {
        static_assert(static_cast<int>(arm64_register::x29) == UC_ARM64_REG_X29);
        static_assert(static_cast<int>(arm64_register::x30) == UC_ARM64_REG_X30);
        static_assert(static_cast<int>(arm64_register::nzcv) == UC_ARM64_REG_NZCV);
        static_assert(static_cast<int>(arm64_register::sp) == UC_ARM64_REG_SP);
        static_assert(static_cast<int>(arm64_register::wsp) == UC_ARM64_REG_WSP);
        static_assert(static_cast<int>(arm64_register::wzr) == UC_ARM64_REG_WZR);
        static_assert(static_cast<int>(arm64_register::xzr) == UC_ARM64_REG_XZR);
        static_assert(static_cast<int>(arm64_register::q0) == UC_ARM64_REG_Q0);
        static_assert(static_cast<int>(arm64_register::q31) == UC_ARM64_REG_Q31);
        static_assert(static_cast<int>(arm64_register::w0) == UC_ARM64_REG_W0);
        static_assert(static_cast<int>(arm64_register::w30) == UC_ARM64_REG_W30);
        static_assert(static_cast<int>(arm64_register::x0) == UC_ARM64_REG_X0);
        static_assert(static_cast<int>(arm64_register::x28) == UC_ARM64_REG_X28);
        static_assert(static_cast<int>(arm64_register::v0) == UC_ARM64_REG_V0);
        static_assert(static_cast<int>(arm64_register::v31) == UC_ARM64_REG_V31);
        static_assert(static_cast<int>(arm64_register::pc) == UC_ARM64_REG_PC);
        static_assert(static_cast<int>(arm64_register::tpidr_el0) == UC_ARM64_REG_TPIDR_EL0);
        static_assert(static_cast<int>(arm64_register::tpidrro_el0) == UC_ARM64_REG_TPIDRRO_EL0);
        static_assert(static_cast<int>(arm64_register::pstate) == UC_ARM64_REG_PSTATE);
        static_assert(static_cast<int>(arm64_register::fpcr) == UC_ARM64_REG_FPCR);
        static_assert(static_cast<int>(arm64_register::fpsr) == UC_ARM64_REG_FPSR);

        constexpr uint32_t svc_mask = 0xFFE0001F;
        constexpr uint32_t svc_opcode = 0xD4000001;
        constexpr uint64_t instruction_size = 4;
        constexpr uint32_t pstate_mode_mask = 0x1F;
        constexpr uint32_t pstate_software_step = 1u << 21;
        constexpr uint32_t cpacr_fp_simd_enabled = 3u << 20;
        constexpr uint64_t cntkctl_el0_virtual_counter_access = 1u << 1;

        // QEMU exception numbers. Unicorn reports EXCP_SWI after advancing PC past the SVC; undefined
        // instructions and BRK keep PC at the faulting instruction.
        constexpr int exception_undefined_instruction = 1;
        constexpr int exception_supervisor_call = 2;
        constexpr int exception_breakpoint = 7;

        std::optional<arm64_exception_class> map_exception_class(const int exception)
        {
            switch (exception)
            {
            case exception_undefined_instruction:
                return arm64_exception_class::undefined_instruction;
            case exception_breakpoint:
                return arm64_exception_class::breakpoint_instruction;
            default:
                return std::nullopt;
            }
        }

        constexpr bool is_in(const arm64_register reg, const arm64_register first, const arm64_register last)
        {
            return reg >= first && reg <= last;
        }

        size_t register_size(const arm64_register reg)
        {
            if (is_in(reg, arm64_register::q0, arm64_register::q31) || is_in(reg, arm64_register::v0, arm64_register::v31))
            {
                return 16;
            }

            if (is_in(reg, arm64_register::w0, arm64_register::w30) || is_in(reg, arm64_register::bcr0, arm64_register::bcr7) ||
                is_in(reg, arm64_register::wcr0, arm64_register::wcr1))
            {
                return sizeof(uint32_t);
            }

            if (is_in(reg, arm64_register::x0, arm64_register::x28) || is_in(reg, arm64_register::bvr0, arm64_register::bvr7) ||
                is_in(reg, arm64_register::wvr0, arm64_register::wvr1))
            {
                return sizeof(uint64_t);
            }

            switch (reg)
            {
            case arm64_register::x29:
            case arm64_register::x30:
            case arm64_register::sp:
            case arm64_register::xzr:
            case arm64_register::pc:
            case arm64_register::tpidr_el0:
            case arm64_register::tpidrro_el0:
            case arm64_register::midr_el1:
            case arm64_register::id_aa64isar0_el1:
            case arm64_register::id_aa64isar1_el1:
                return sizeof(uint64_t);
            case arm64_register::wsp:
            case arm64_register::wzr:
            case arm64_register::nzcv:
            case arm64_register::pstate:
            case arm64_register::fpcr:
            case arm64_register::fpsr:
                return sizeof(uint32_t);
            default:
                throw std::invalid_argument("Unsupported Unicorn ARM64 register: " + std::to_string(static_cast<int>(reg)));
            }
        }

        struct debug_register
        {
            uint32_t index{};
            uint32_t op2{};
            bool watchpoint{};
        };

        // DBG{B,W}{V,C}R<n>_EL1 are encoded as op0=2, op1=0, CRn=0, CRm=n with op2 selecting the kind.
        // CRm and op2 of the identification registers (op0 = 3, op1 = 0, CRn = 0).
        std::optional<std::pair<uint32_t, uint32_t>> map_id_register(const arm64_register reg)
        {
            switch (reg)
            {
            case arm64_register::midr_el1:
                return std::pair{0u, 0u};
            case arm64_register::id_aa64isar0_el1:
                return std::pair{6u, 0u};
            case arm64_register::id_aa64isar1_el1:
                return std::pair{6u, 1u};
            default:
                return std::nullopt;
            }
        }

        std::optional<debug_register> map_debug_register(const arm64_register reg)
        {
            const auto offset = [reg](const arm64_register first) {
                return static_cast<uint32_t>(static_cast<int>(reg) - static_cast<int>(first));
            };

            if (is_in(reg, arm64_register::bvr0, arm64_register::bvr7))
            {
                return debug_register{.index = offset(arm64_register::bvr0), .op2 = 4};
            }
            if (is_in(reg, arm64_register::bcr0, arm64_register::bcr7))
            {
                return debug_register{.index = offset(arm64_register::bcr0), .op2 = 5};
            }
            if (is_in(reg, arm64_register::wvr0, arm64_register::wvr1))
            {
                return debug_register{.index = offset(arm64_register::wvr0), .op2 = 6, .watchpoint = true};
            }
            if (is_in(reg, arm64_register::wcr0, arm64_register::wcr1))
            {
                return debug_register{.index = offset(arm64_register::wcr0), .op2 = 7, .watchpoint = true};
            }

            return std::nullopt;
        }

        std::optional<arm64_register> full_width_register(const arm64_register reg)
        {
            if (is_in(reg, arm64_register::w0, arm64_register::w28))
            {
                return static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + static_cast<int>(reg) -
                                                   static_cast<int>(arm64_register::w0));
            }

            switch (reg)
            {
            case arm64_register::w29:
                return arm64_register::x29;
            case arm64_register::w30:
                return arm64_register::x30;
            case arm64_register::wsp:
                return arm64_register::sp;
            default:
                return std::nullopt;
            }
        }

        class unicorn_arm64_emulator final : public unicorn_emulator<arm64_emulator>
        {
          public:
            unicorn_arm64_emulator()
                : unicorn_emulator(UC_ARCH_ARM64, UC_MODE_ARM, UC_CPU_ARM64_MAX)
            {
                // Unicorn resets into EL1 with FP/SIMD trapped; a Windows thread runs at EL0 with both enabled.
                constexpr uint32_t el0_pstate = 0;
                uce(uc_reg_write(*this, UC_ARM64_REG_PSTATE, &el0_pstate));
                uce(uc_reg_write(*this, UC_ARM64_REG_CPACR_EL1, &cpacr_fp_simd_enabled));
                // Windows lets user mode read CNTVCT_EL0 directly (CNTKCTL_EL1.EL0VCTEN).
                this->write_system_register(3, 0, 14, 1, 0, cntkctl_el0_virtual_counter_access);

                const auto debug_features = this->read_system_register(3, 0, 0, 5, 0);
                this->breakpoint_count_ = static_cast<size_t>(((debug_features >> 12) & 0xF) + 1);
                this->watchpoint_count_ = static_cast<size_t>(((debug_features >> 20) & 0xF) + 1);

                function_wrapper<void, uc_engine*, int> wrapper(
                    [this](uc_engine*, const int exception) { this->run_hook_guarded([&] { this->handle_exception(exception); }); });

                unicorn_hook hook{*this};
                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INTR, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<pointer_type>::max()));
                this->add_hook(std::move(wrapper), std::move(hook));
            }

            size_t read_raw_register(const int reg, void* value, const size_t size) override
            {
                const auto id = static_cast<arm64_register>(reg);
                const auto required = validated_register_size(id, value, size);

                if (const auto encoding = map_id_register(id))
                {
                    std::memset(value, 0, size);
                    const auto current = this->read_system_register(3, 0, 0, encoding->first, encoding->second);
                    std::memcpy(value, &current, required);
                    return required;
                }

                if (const auto debug = map_debug_register(id))
                {
                    std::memset(value, 0, size);
                    if (this->has_debug_slot(*debug))
                    {
                        const auto current = this->read_system_register(2, 0, 0, debug->index, debug->op2);
                        std::memcpy(value, &current, required);
                    }
                    return required;
                }

                return unicorn_emulator::read_raw_register(reg, value, size);
            }

            size_t write_raw_register(const int reg, const void* value, const size_t size) override
            {
                const auto id = static_cast<arm64_register>(reg);
                const auto required = validated_register_size(id, value, size);

                if (map_id_register(id))
                {
                    throw std::invalid_argument("ARM64 identification registers are read-only");
                }

                if (id == arm64_register::pstate)
                {
                    uint32_t pstate{};
                    std::memcpy(&pstate, value, sizeof(pstate));
                    if ((pstate & pstate_mode_mask) != 0)
                    {
                        throw std::invalid_argument("Unicorn ARM64 only exposes an AArch64 EL0 CPU");
                    }

                    if ((pstate & pstate_software_step) != 0 && this->is_emulating())
                    {
                        unicorn_emulator::stop();
                    }
                }

                if (const auto debug = map_debug_register(id))
                {
                    uint64_t debug_value{};
                    std::memcpy(&debug_value, value, required);
                    if (this->has_debug_slot(*debug))
                    {
                        this->write_system_register(2, 0, 0, debug->index, debug->op2, debug_value);
                    }
                    else if (debug_value != 0)
                    {
                        throw std::invalid_argument("ARM64 context uses a debug register not implemented by the virtual processor");
                    }
                    return required;
                }

                // Unicorn merges a W-register write into the old X value; AArch64 zero-extends it.
                if (const auto full = full_width_register(id))
                {
                    uint32_t narrow{};
                    std::memcpy(&narrow, value, sizeof(narrow));
                    const uint64_t extended = narrow;
                    unicorn_emulator::write_raw_register(static_cast<int>(*full), &extended, sizeof(extended));
                    return required;
                }

                return unicorn_emulator::write_raw_register(reg, value, required);
            }

            emulator_hook* hook_instruction(const int instruction, instruction_hook_callback callback) override
            {
                if (instruction == static_cast<int>(arm64_hookable_instructions::read_counter) && callback)
                {
                    return this->hook_counter_read(std::move(callback));
                }

                if (instruction != static_cast<int>(arm64_hookable_instructions::svc) || !callback)
                {
                    throw std::invalid_argument("Unicorn ARM64 requires a nonempty SVC or counter instruction hook");
                }
                if (this->svc_hook_)
                {
                    throw std::logic_error("Unicorn ARM64 supports one SVC instruction hook");
                }

                this->svc_hook_ = std::make_unique<svc_hook>(svc_hook{std::move(callback)});
                return reinterpret_cast<emulator_hook*>(this->svc_hook_.get());
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                if (!callback)
                {
                    throw std::invalid_argument("Unicorn ARM64 requires a nonempty interrupt hook");
                }

                auto& hook = this->interrupt_hooks_.emplace_back(std::make_unique<interrupt_hook>(interrupt_hook{std::move(callback)}));
                return reinterpret_cast<emulator_hook*>(hook.get());
            }

            void delete_hook(emulator_hook* hook) override
            {
                if (this->svc_hook_ && hook == reinterpret_cast<emulator_hook*>(this->svc_hook_.get()))
                {
                    this->svc_hook_.reset();
                    return;
                }

                const auto interrupt = std::ranges::find_if(this->interrupt_hooks_, [&](const std::unique_ptr<interrupt_hook>& entry) {
                    return reinterpret_cast<emulator_hook*>(entry.get()) == hook;
                });
                if (interrupt != this->interrupt_hooks_.end())
                {
                    this->interrupt_hooks_.erase(interrupt);
                    return;
                }

                unicorn_emulator::delete_hook(hook);
            }

            void start(const size_t count) override
            {
                this->stop_requested_ = false;
                auto remaining = count;
                for (;;)
                {
                    // A step handler may request the next step, so the request is checked again after every step.
                    if (this->run_software_step())
                    {
                        if (remaining == 1 || this->has_violation() || this->stop_requested_)
                        {
                            return;
                        }
                        remaining -= remaining != 0 ? 1 : 0;
                        continue;
                    }

                    unicorn_emulator::start(remaining);

                    // A context continued with PSTATE.SS ends the run so the step begins at its first instruction.
                    if (remaining != 0 || this->has_violation() || this->stop_requested_ || !this->software_step_requested())
                    {
                        return;
                    }
                }
            }

            void stop() override
            {
                this->stop_requested_ = true;
                unicorn_emulator::stop();
            }

            std::string get_name() const override
            {
                return "Unicorn Engine ARM64";
            }

          private:
            struct svc_hook
            {
                instruction_hook_callback callback;
            };

            struct interrupt_hook
            {
                interrupt_hook_callback callback;
            };

            std::unique_ptr<svc_hook> svc_hook_{};
            size_t exception_count_{};
            bool stop_requested_{};
            std::vector<std::unique_ptr<interrupt_hook>> interrupt_hooks_{};
            size_t breakpoint_count_{};
            size_t watchpoint_count_{};

            // QEMU only steps with MDSCR_EL1.SS, which EL0 cannot own here, so a user PSTATE.SS is honoured by
            // running one instruction and reporting the software step.
            bool software_step_requested()
            {
                return (this->reg<uint32_t>(arm64_register::pstate) & pstate_software_step) != 0;
            }

            bool run_software_step()
            {
                if (!this->software_step_requested())
                {
                    return false;
                }

                const auto pstate = this->reg<uint32_t>(arm64_register::pstate);
                const auto pc = this->read_instruction_pointer();

                this->reg<uint32_t>(arm64_register::pstate, pstate & ~pstate_software_step);
                const auto exceptions = this->exception_count_;
                unicorn_emulator::start(1);
                if (this->has_violation() || this->exception_count_ != exceptions)
                {
                    return true;
                }

                if (this->stop_requested_)
                {
                    // A hook stopped the CPU before the instruction ran, so the step is still pending. An instruction
                    // that branches to itself also ends here; running it again has the same effect.
                    if (this->read_instruction_pointer() == pc)
                    {
                        this->reg<uint32_t>(arm64_register::pstate, this->reg<uint32_t>(arm64_register::pstate) | pstate_software_step);
                    }
                    return true;
                }

                if (this->interrupt_hooks_.empty())
                {
                    throw std::runtime_error("Unicorn ARM64 software step without an interrupt hook");
                }
                this->report_exception(arm64_exception_class::software_step);
                return true;
            }

            static size_t validated_register_size(const arm64_register reg, const void* buffer, const size_t size)
            {
                const auto required = register_size(reg);
                if (!buffer || size < required)
                {
                    throw std::invalid_argument("Unicorn ARM64 register buffer is too small or null");
                }

                return required;
            }

            // Unicorn translates an MRS with the first matching hook only, so one hook serves every counter.
            emulator_hook* hook_counter_read(instruction_hook_callback callback)
            {
                function_wrapper<uint32_t, uc_engine*, uc_arm64_reg, const uc_arm64_cp_reg*> wrapper(
                    [c = std::move(callback), this](uc_engine*, const uc_arm64_reg destination, const uc_arm64_cp_reg* reg) -> uint32_t {
                        const auto is_virtual_counter = reg->op0 == 3 && reg->op1 == 3 && reg->crn == 14 && reg->crm == 0 && reg->op2 == 2;
                        const auto is_cycle_counter = reg->op0 == 3 && reg->op1 == 3 && reg->crn == 9 && reg->crm == 13 && reg->op2 == 0;
                        if (!is_virtual_counter && !is_cycle_counter)
                        {
                            return 0;
                        }

                        auto continuation = instruction_hook_continuation::skip_instruction;
                        this->run_hook_guarded([&] { continuation = c(*this, static_cast<uint64_t>(destination)); });
                        return continuation == instruction_hook_continuation::run_instruction ? 0 : 1;
                    });

                unicorn_hook hook{*this};
                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<pointer_type>::max(), UC_ARM64_INS_MRS));
                return this->add_hook(std::move(wrapper), std::move(hook));
            }

            bool has_debug_slot(const debug_register& reg) const
            {
                return reg.index < (reg.watchpoint ? this->watchpoint_count_ : this->breakpoint_count_);
            }

            uint64_t read_system_register(const uint32_t op0, const uint32_t op1, const uint32_t crn, const uint32_t crm,
                                          const uint32_t op2) const
            {
                uc_arm64_cp_reg reg{.crn = crn, .crm = crm, .op0 = op0, .op1 = op1, .op2 = op2, .val = 0};
                uce(uc_reg_read(*this, UC_ARM64_REG_CP_REG, &reg));
                return reg.val;
            }

            void write_system_register(const uint32_t op0, const uint32_t op1, const uint32_t crn, const uint32_t crm, const uint32_t op2,
                                       const uint64_t value)
            {
                uc_arm64_cp_reg reg{.crn = crn, .crm = crm, .op0 = op0, .op1 = op1, .op2 = op2, .val = value};
                uce(uc_reg_write(*this, UC_ARM64_REG_CP_REG, &reg));
            }

            void report_exception(const arm64_exception_class exception_class)
            {
                std::vector<interrupt_hook_callback> callbacks{};
                callbacks.reserve(this->interrupt_hooks_.size());
                for (const auto& hook : this->interrupt_hooks_)
                {
                    callbacks.push_back(hook->callback);
                }

                for (const auto& callback : callbacks)
                {
                    callback(*this, static_cast<int>(exception_class));
                }
            }

            void handle_exception(const int exception)
            {
                ++this->exception_count_;
                const auto exception_class = map_exception_class(exception);
                if (exception_class && !this->interrupt_hooks_.empty())
                {
                    this->report_exception(*exception_class);
                    return;
                }

                if (exception != exception_supervisor_call)
                {
                    std::ostringstream error{};
                    error << "Unhandled Unicorn ARM64 guest exception " << exception << " at PC 0x" << std::hex
                          << this->read_instruction_pointer();
                    throw std::runtime_error(error.str());
                }

                this->handle_supervisor_call();
            }

            void handle_supervisor_call()
            {
                const auto return_pc = this->read_instruction_pointer();
                uint32_t instruction{};
                if (return_pc < instruction_size ||
                    !this->try_read_memory(return_pc - instruction_size, &instruction, sizeof(instruction)) ||
                    (instruction & svc_mask) != svc_opcode)
                {
                    std::ostringstream error{};
                    error << "Unicorn ARM64 supervisor call does not follow an SVC instruction at PC 0x" << std::hex << return_pc;
                    throw std::runtime_error(error.str());
                }

                // Hooks see the SVC address in PC, as on the WHP backend and as x86 hooks see SYSCALL.
                this->reg(arm64_register::pc, return_pc - instruction_size);
                if (!this->svc_hook_)
                {
                    throw std::runtime_error("Unicorn ARM64 guest issued SVC without a syscall hook");
                }

                auto callback = this->svc_hook_->callback;
                switch (callback(*this, (instruction >> 5) & 0xFFFF))
                {
                case instruction_hook_continuation::skip_instruction:
                    this->reg(arm64_register::pc, this->read_instruction_pointer() + instruction_size);
                    return;
                case instruction_hook_continuation::finalized_instruction_pointer:
                    return;
                case instruction_hook_continuation::run_instruction:
                    throw std::runtime_error("Unicorn ARM64 SVC hooks must handle the syscall (skip or finalized PC)");
                default:
                    throw std::runtime_error("Invalid Unicorn ARM64 SVC hook continuation");
                }
            }
        };
    }

    std::unique_ptr<arm64_emulator> create_arm64_emulator()
    {
        if (!uc_arch_supported(UC_ARCH_ARM64))
        {
            throw std::runtime_error("This Unicorn build does not include the AArch64 guest; configure UNICORN_ARCH with aarch64");
        }

        return std::make_unique<unicorn_arm64_emulator>();
    }
} // namespace sogen::unicorn
