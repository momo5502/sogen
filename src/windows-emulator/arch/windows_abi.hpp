#pragma once

#include "syscall_abi.hpp"

#include <arch_emulator.hpp>
#include <platform/arm64_context.hpp>

namespace sogen
{
    enum class windows_guest_architecture : uint8_t
    {
        x86_64,
        arm64,
    };

    struct native_thread_bootstrap
    {
        uint64_t stack_base{};
        uint64_t stack_size{};
        uint64_t teb{};
        uint64_t start_address{};
        uint64_t argument{};
        uint64_t rtl_user_thread_start{};
        uint64_t ldr_initialize_thunk{};
        uint64_t ntdll_image_base{};
    };

    struct windows_arch_abi
    {
        virtual ~windows_arch_abi() = default;

        virtual windows_guest_architecture architecture() const = 0;
        virtual const arch_syscall_abi& syscall_abi() const = 0;
        virtual uint64_t instruction_pointer(cpu_interface& cpu) const = 0;
        virtual void instruction_pointer(cpu_interface& cpu, uint64_t address) const = 0;
        virtual uint64_t stack_pointer(cpu_interface& cpu) const = 0;
        virtual void stack_pointer(cpu_interface& cpu, uint64_t address) const = 0;
        virtual uint64_t return_value(cpu_interface& cpu) const = 0;
        virtual void return_value(cpu_interface& cpu, uint64_t value) const = 0;
        virtual uint32_t raw_syscall_id(cpu_interface& cpu) const = 0;
        virtual uint32_t syscall_id(uint32_t raw_id) const = 0;
        virtual uint64_t syscall_argument(cpu_interface& cpu, size_t index) const = 0;
        virtual uint64_t function_argument(cpu_interface& cpu, size_t index) const = 0;
        virtual void install_thread_environment(cpu_interface& cpu, uint64_t teb) const = 0;
        virtual uint64_t initialize_native_thread(cpu_interface& cpu, const native_thread_bootstrap& bootstrap) const = 0;
        virtual void redirect_instrumentation_callback(cpu_interface& cpu, uint64_t callback) const = 0;

        void rewind_syscall(cpu_interface& cpu, uint64_t address) const;
    };

    const windows_arch_abi& windows_abi_for(windows_guest_architecture architecture);
    windows_guest_architecture guest_architecture_of(const guest_machine& machine);
}
