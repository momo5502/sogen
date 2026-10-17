#define UNICORN_EMULATOR_IMPL
#include "unicorn_x86_64_emulator.hpp"
#include "unicorn_emulator.hpp"

namespace sogen::unicorn
{
    namespace
    {
        static_assert(static_cast<uint32_t>(x86_register::end) == UC_X86_REG_ENDING);

        constexpr auto IA32_FS_BASE_MSR = 0xC0000100;
        constexpr auto IA32_GS_BASE_MSR = 0xC0000101;
        constexpr uint64_t syscall_instruction_size = 2;

        struct msr_value
        {
            uint64_t id{};
            uint64_t value{};
        };

        uc_x86_insn map_hookable_instruction(const x86_hookable_instructions instruction)
        {
            switch (instruction)
            {
            case x86_hookable_instructions::syscall:
                return UC_X86_INS_SYSCALL;
            case x86_hookable_instructions::cpuid:
                return UC_X86_INS_CPUID;
            case x86_hookable_instructions::rdtsc:
                return UC_X86_INS_RDTSC;
            case x86_hookable_instructions::rdtscp:
                return UC_X86_INS_RDTSCP;
            default:
                throw std::runtime_error("Bad instruction for mapping");
            }
        }

        class unicorn_x86_64_emulator final : public unicorn_emulator<x86_64_emulator>
        {
          public:
            unicorn_x86_64_emulator()
                : unicorn_emulator(UC_ARCH_X86, UC_MODE_64)
            {
            }

            void load_gdt(const pointer_type address, const uint32_t limit) override
            {
                uc_x86_mmr gdt{};
                gdt.base = address;
                gdt.limit = limit;

                this->write_register(x86_register::gdtr, &gdt, sizeof(gdt));
            }

            void set_segment_base(const x86_register base, const pointer_type value) override
            {
                msr_value msr_val{
                    .id = 0,
                    .value = value,
                };

                switch (base)
                {
                case x86_register::fs:
                case x86_register::fs_base:
                    msr_val.id = IA32_FS_BASE_MSR;
                    break;
                case x86_register::gs:
                case x86_register::gs_base:
                    msr_val.id = IA32_GS_BASE_MSR;
                    break;
                default:
                    return;
                }

                this->write_register(x86_register::msr, &msr_val, sizeof(msr_val));
            }

            pointer_type get_segment_base(const x86_register base) override
            {
                msr_value msr_val{};

                switch (base)
                {
                case x86_register::fs:
                case x86_register::fs_base:
                    msr_val.id = IA32_FS_BASE_MSR;
                    break;
                case x86_register::gs:
                case x86_register::gs_base:
                    msr_val.id = IA32_GS_BASE_MSR;
                    break;
                default:
                    return 0;
                }

                size_t result_size = sizeof(msr_value);
                uce(uc_reg_read2(*this, static_cast<int>(x86_register::msr), &msr_val, &result_size));

                return msr_val.value;
            }

            bool read_descriptor_table(const int reg, descriptor_table_register& table) override
            {
                if (reg != static_cast<int>(x86_register::gdtr) && reg != static_cast<int>(x86_register::idtr))
                {
                    return false;
                }

                uc_x86_mmr gdt{};

                this->read_register(x86_register::gdtr, &gdt, sizeof(gdt));

                table.base = gdt.base;
                table.limit = gdt.limit;
                return true;
            }

            emulator_hook* hook_instruction(const int instruction_type, instruction_hook_callback callback) override
            {
                unicorn_hook hook{*this};

                const auto inst_type = static_cast<x86_hookable_instructions>(instruction_type);

                if (inst_type == x86_hookable_instructions::invalid)
                {
                    function_wrapper<int, uc_engine*> wrapper([c = std::move(callback), this](uc_engine*) {
                        return (c(*this, 0) == instruction_hook_continuation::skip_instruction) ? 1 : 0;
                    });

                    uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN_INVALID, wrapper.get_function(), wrapper.get_user_data(), 0,
                                    std::numeric_limits<pointer_type>::max()));
                    return this->add_hook(std::move(wrapper), std::move(hook));
                }

                if (inst_type == x86_hookable_instructions::syscall)
                {
                    function_wrapper<void, uc_engine*> wrapper([c = std::move(callback), this](uc_engine*) {
                        const auto continuation = c(*this, 0);
                        if (continuation == instruction_hook_continuation::finalized_instruction_pointer)
                        {
                            this->reg(x86_register::rip, this->read_instruction_pointer() - syscall_instruction_size);
                        }
                    });

                    const auto uc_instruction = map_hookable_instruction(inst_type);
                    uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN, wrapper.get_function(), wrapper.get_user_data(), 0,
                                    std::numeric_limits<pointer_type>::max(), uc_instruction));

                    return this->add_hook(std::move(wrapper), std::move(hook));
                }

                function_wrapper<int, uc_engine*> wrapper([c = std::move(callback), this](uc_engine*) {
                    return (c(*this, 0) == instruction_hook_continuation::skip_instruction) ? 1 : 0;
                });

                const auto uc_instruction = map_hookable_instruction(inst_type);
                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<pointer_type>::max(), uc_instruction));

                return this->add_hook(std::move(wrapper), std::move(hook));
            }

            emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
            {
                function_wrapper<void, uc_engine*, int> wrapper(
                    [c = std::move(callback), this](uc_engine*, const int interrupt_type) { c(*this, interrupt_type); });

                unicorn_hook hook{*this};

                uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INTR, wrapper.get_function(), wrapper.get_user_data(), 0,
                                std::numeric_limits<pointer_type>::max()));

                return this->add_hook(std::move(wrapper), std::move(hook));
            }

            std::string get_name() const override
            {
                return "Unicorn Engine";
            }
        };
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
    {
        return std::make_unique<unicorn_x86_64_emulator>();
    }
} // namespace sogen::unicorn
