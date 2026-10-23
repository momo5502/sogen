#include "../std_include.hpp"
#include "windows_abi.hpp"

#include "../cpu_context.hpp"
#include "../emulator_utils.hpp"

namespace sogen
{
    namespace
    {
        template <typename Cpu>
        Cpu& require_cpu(cpu_interface& cpu)
        {
            auto* typed = dynamic_cast<Cpu*>(&cpu);
            if (!typed)
            {
                throw std::invalid_argument("Windows ABI does not match the guest CPU architecture");
            }
            return *typed;
        }

        uint64_t initial_stack_top(const native_thread_bootstrap& bootstrap, const uint64_t required)
        {
            if (bootstrap.stack_size < required || bootstrap.stack_base > UINT64_MAX - bootstrap.stack_size || !bootstrap.teb)
            {
                throw std::invalid_argument("Invalid native Windows thread bootstrap");
            }
            return bootstrap.stack_base + bootstrap.stack_size;
        }

        struct x86_64_windows_abi final : windows_arch_abi
        {
            windows_guest_architecture architecture() const override
            {
                return windows_guest_architecture::x86_64;
            }

            const arch_syscall_abi& syscall_abi() const override
            {
                return x86_64_syscall_abi_instance();
            }

            uint64_t instruction_pointer(cpu_interface& cpu) const override
            {
                return require_cpu<x86_64_cpu>(cpu).read_instruction_pointer();
            }

            void instruction_pointer(cpu_interface& cpu, const uint64_t address) const override
            {
                require_cpu<x86_64_cpu>(cpu).reg(x86_register::rip, address);
            }

            uint64_t stack_pointer(cpu_interface& cpu) const override
            {
                return require_cpu<x86_64_cpu>(cpu).read_stack_pointer();
            }

            void stack_pointer(cpu_interface& cpu, const uint64_t address) const override
            {
                require_cpu<x86_64_cpu>(cpu).reg(x86_register::rsp, address);
            }

            uint64_t return_value(cpu_interface& cpu) const override
            {
                return require_cpu<x86_64_cpu>(cpu).reg(x86_register::rax);
            }

            void return_value(cpu_interface& cpu, const uint64_t value) const override
            {
                require_cpu<x86_64_cpu>(cpu).reg(x86_register::rax, value);
            }

            uint32_t raw_syscall_id(cpu_interface& cpu) const override
            {
                return require_cpu<x86_64_cpu>(cpu).reg<uint32_t>(x86_register::eax);
            }

            uint32_t syscall_id(const uint32_t raw_id) const override
            {
                return raw_id & 0x3FFF;
            }

            uint64_t syscall_argument(cpu_interface& cpu, const size_t index) const override
            {
                return get_function_argument_x64_syscall(require_cpu<x86_64_cpu>(cpu), index);
            }

            uint64_t function_argument(cpu_interface& cpu, const size_t index) const override
            {
                return get_function_argument(require_cpu<x86_64_cpu>(cpu), index);
            }

            void install_thread_environment(cpu_interface& cpu, const uint64_t teb) const override
            {
                require_cpu<x86_64_cpu>(cpu).set_segment_base(x86_register::gs, teb);
            }

            uint64_t initialize_native_thread(cpu_interface& cpu, const native_thread_bootstrap& bootstrap) const override
            {
                auto& native = require_cpu<x86_64_cpu>(cpu);
                const auto top = initial_stack_top(bootstrap, sizeof(CONTEXT64) + 0x40);
                const auto initial_sp = align_down(top - 0x10, 0x10) + 8;
                const auto context_address = align_down(initial_sp - sizeof(CONTEXT64), alignof(CONTEXT64));
                const auto loader_sp = align_down(context_address - 0x10, 0x10) + 8;

                install_thread_environment(cpu, bootstrap.teb);
                native.reg<uint16_t>(x86_register::fpcw, 0x037F);
                native.reg<uint32_t>(x86_register::mxcsr, 0x1F80);
                native.reg(x86_register::rsp, initial_sp);

                CONTEXT64 context{};
                context.ContextFlags = CONTEXT64_ALL;
                cpu_context::save(native, context);
                context.Rip = bootstrap.rtl_user_thread_start;
                context.Rcx = bootstrap.start_address;
                context.Rdx = bootstrap.argument;
                native.write_memory(context_address, context);
                native.reg(x86_register::rsp, loader_sp);
                native.reg(x86_register::rcx, context_address);
                native.reg(x86_register::rdx, bootstrap.ntdll_image_base);
                native.reg(x86_register::rip, bootstrap.ldr_initialize_thunk);
                return context_address;
            }

            void redirect_instrumentation_callback(cpu_interface& cpu, const uint64_t callback) const override
            {
                const auto previous_ip = instruction_pointer(cpu);
                rewind_syscall(cpu, callback);
                require_cpu<x86_64_cpu>(cpu).reg(x86_register::r10, previous_ip);
            }
        };

        struct arm64_windows_abi final : windows_arch_abi
        {
            windows_guest_architecture architecture() const override
            {
                return windows_guest_architecture::arm64;
            }

            const arch_syscall_abi& syscall_abi() const override
            {
                return arm64_syscall_abi_instance();
            }

            uint64_t instruction_pointer(cpu_interface& cpu) const override
            {
                return require_cpu<arm64_cpu>(cpu).read_instruction_pointer();
            }

            void instruction_pointer(cpu_interface& cpu, const uint64_t address) const override
            {
                require_cpu<arm64_cpu>(cpu).reg(arm64_register::pc, address);
            }

            uint64_t stack_pointer(cpu_interface& cpu) const override
            {
                return require_cpu<arm64_cpu>(cpu).read_stack_pointer();
            }

            void stack_pointer(cpu_interface& cpu, const uint64_t address) const override
            {
                require_cpu<arm64_cpu>(cpu).reg(arm64_register::sp, address);
            }

            uint64_t return_value(cpu_interface& cpu) const override
            {
                return require_cpu<arm64_cpu>(cpu).reg(arm64_register::x0);
            }

            void return_value(cpu_interface& cpu, const uint64_t value) const override
            {
                require_cpu<arm64_cpu>(cpu).reg(arm64_register::x0, value);
            }

            uint32_t raw_syscall_id(cpu_interface& cpu) const override
            {
                auto& native = require_cpu<arm64_cpu>(cpu);
                const auto instruction = native.read_memory<uint32_t>(native.read_instruction_pointer());
                if ((instruction & 0xFFE0001F) != 0xD4000001)
                {
                    throw std::runtime_error("ARM64 syscall PC does not address an SVC instruction");
                }
                return (instruction >> 5) & 0xFFFF;
            }

            uint32_t syscall_id(const uint32_t raw_id) const override
            {
                return raw_id;
            }

            uint64_t syscall_argument(cpu_interface& cpu, const size_t index) const override
            {
                return function_argument(cpu, index);
            }

            uint64_t function_argument(cpu_interface& cpu, const size_t index) const override
            {
                auto& native = require_cpu<arm64_cpu>(cpu);
                if (index < 8)
                {
                    return native.reg(static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + index));
                }
                const auto offset = index - 8;
                if (offset > (UINT64_MAX - native.read_stack_pointer()) / sizeof(uint64_t))
                {
                    throw std::out_of_range("ARM64 argument index exceeds the guest address space");
                }
                return native.read_memory<uint64_t>(native.read_stack_pointer() + offset * sizeof(uint64_t));
            }

            void install_thread_environment(cpu_interface& cpu, const uint64_t teb) const override
            {
                require_cpu<arm64_cpu>(cpu).reg(arm64_register::x18, teb);
            }

            uint64_t initialize_native_thread(cpu_interface& cpu, const native_thread_bootstrap& bootstrap) const override
            {
                auto& native = require_cpu<arm64_cpu>(cpu);
                const auto top = initial_stack_top(bootstrap, sizeof(arm64_context) + 0x30);
                // The RtlUserThreadStart establisher frame must lie strictly below StackBase for exception dispatch.
                const auto initial_sp = align_down(top - 0x10, 0x10);
                const auto context_address = align_down(initial_sp - sizeof(arm64_context), alignof(arm64_context));

                install_thread_environment(cpu, bootstrap.teb);
                native.reg<uint32_t>(arm64_register::fpcr, 0);
                native.reg<uint32_t>(arm64_register::fpsr, 0);
                native.reg(arm64_register::sp, initial_sp);

                arm64_context context{};
                context.ContextFlags = arm64_context_all;
                cpu_context::save(native, context);
                context.Pc = bootstrap.rtl_user_thread_start;
                context.X[0] = bootstrap.start_address;
                context.X[1] = bootstrap.argument;
                native.write_memory(context_address, context);
                native.reg(arm64_register::sp, context_address);
                native.reg(arm64_register::x0, context_address);
                native.reg(arm64_register::x1, bootstrap.ntdll_image_base);
                native.reg(arm64_register::pc, bootstrap.ldr_initialize_thunk);
                return context_address;
            }

            // Windows on ARM enters the callback with the syscall's return address (after the SVC) in X16.
            void redirect_instrumentation_callback(cpu_interface& cpu, const uint64_t callback) const override
            {
                const auto return_address = instruction_pointer(cpu) + syscall_abi().syscall_instruction_size();
                rewind_syscall(cpu, callback);
                require_cpu<arm64_cpu>(cpu).reg(arm64_register::x16, return_address);
            }
        };
    }

    void windows_arch_abi::rewind_syscall(cpu_interface& cpu, const uint64_t address) const
    {
        const auto size = syscall_abi().syscall_instruction_size();
        if (address < size)
        {
            throw std::out_of_range("Syscall instruction rewind underflows the guest address");
        }
        instruction_pointer(cpu, address - size);
    }

    const windows_arch_abi& windows_abi_for(const windows_guest_architecture architecture)
    {
        static const x86_64_windows_abi x64{};
        static const arm64_windows_abi arm64{};
        switch (architecture)
        {
        case windows_guest_architecture::x86_64:
            return x64;
        case windows_guest_architecture::arm64:
            return arm64;
        default:
            throw std::invalid_argument("Unsupported Windows guest architecture");
        }
    }

    windows_guest_architecture guest_architecture_of(const guest_machine& machine)
    {
        if (dynamic_cast<const x86_64_emulator*>(&machine))
        {
            return windows_guest_architecture::x86_64;
        }
        if (dynamic_cast<const arm64_emulator*>(&machine))
        {
            return windows_guest_architecture::arm64;
        }

        throw std::invalid_argument("Unsupported Windows guest machine: " + machine.get_name());
    }
}
