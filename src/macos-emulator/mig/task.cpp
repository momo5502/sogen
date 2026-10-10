#include "macos_mig.hpp"
#include "../macos_emulator.hpp"

namespace sogen
{
    std::vector<std::byte> mig_semaphore_create(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        return make_mig_port_reply(request, c.emu_ref.mach_ports.create_kernel_object(mach_port_object::semaphore));
    }

    // Restartable ranges only matter when a thread is preempted inside one; the emulator never interrupts a thread
    // in the middle of these sequences.
    std::vector<std::byte> mig_task_restartable_ranges_register(const macos_syscall_context&, const std::span<const std::byte> request)
    {
        return make_mig_return_code_reply(request, macos_kern_return::KERN_SUCCESS);
    }

    // There is no launchd, so the bootstrap port is null, as for a process started without one.
    std::vector<std::byte> mig_task_get_special_port(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        constexpr int32_t task_kernel_port = 1;
        constexpr int32_t task_host_port = 2;
        constexpr int32_t task_name_port = 3;
        constexpr int32_t task_bootstrap_port = 4;
        constexpr uint32_t mach_port_null = 0;

        switch (read_mig_field<int32_t>(request, MIG_BODY_OFFSET))
        {
        case task_kernel_port:
        case task_name_port:
            return make_mig_port_reply(request, macos_mach_ports::TASK_PORT_NAME);
        case task_host_port:
            return make_mig_port_reply(request, macos_mach_ports::HOST_PORT_NAME);
        case task_bootstrap_port:
            return make_mig_port_reply(request, mach_port_null);
        default:
            c.emu_ref.log.error("Unhandled task_get_special_port(%d)\n", read_mig_field<int32_t>(request, MIG_BODY_OFFSET));
            return {};
        }
    }
}
