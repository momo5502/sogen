#include "macos_mig.hpp"
#include "../macos_emulator.hpp"

#include <array>
#include <cstring>

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

    // There is no launchd: the bootstrap port is a send right to a port nobody receives on.
    std::vector<std::byte> mig_task_get_special_port(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        constexpr int32_t task_kernel_port = 1;
        constexpr int32_t task_host_port = 2;
        constexpr int32_t task_name_port = 3;
        constexpr int32_t task_bootstrap_port = 4;

        switch (read_mig_field<int32_t>(request, MIG_BODY_OFFSET))
        {
        case task_kernel_port:
        case task_name_port:
            return make_mig_port_reply(request, macos_mach_ports::TASK_PORT_NAME);
        case task_host_port:
            return make_mig_port_reply(request, macos_mach_ports::HOST_PORT_NAME);
        case task_bootstrap_port:
            return make_mig_port_reply(request, macos_mach_ports::BOOTSTRAP_PORT_NAME);
        default:
            c.emu_ref.log.error("Unhandled task_get_special_port(%d)\n", read_mig_field<int32_t>(request, MIG_BODY_OFFSET));
            return {};
        }
    }

    // The request carries the new port in a descriptor, then the NDR record and the special port index.
    std::vector<std::byte> mig_task_set_special_port(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        constexpr size_t descriptor_size = 12;
        constexpr size_t which_offset = sizeof(mach_msg_header) + sizeof(uint32_t) + descriptor_size + 8;

        c.emu_ref.log.log("task_set_special_port(%d)\n", read_mig_field<int32_t>(request, which_offset));
        return make_mig_return_code_reply(request, macos_kern_return::KERN_SUCCESS);
    }

    std::vector<std::byte> mig_task_info(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        constexpr int32_t task_audit_token = 15;
        constexpr uint32_t audit_session_id = 100000;

        const auto flavor = read_mig_field<int32_t>(request, MIG_BODY_OFFSET);
        const auto requested_count = read_mig_field<uint32_t>(request, MIG_BODY_OFFSET + sizeof(int32_t));

        if (flavor != task_audit_token)
        {
            c.emu_ref.log.error("Unhandled task_info(flavor %d, count %u)\n", flavor, requested_count);
            return {};
        }

        // audit_token_t: audit user, effective user and group, real user and group, pid, audit session, pid version.
        const std::array<uint32_t, 8> audit_token{USER_ID,  USER_ID,          GROUP_ID,         USER_ID,
                                                  GROUP_ID, MACOS_PROCESS_ID, audit_session_id, MACOS_PROCESS_ID_VERSION};

        if (requested_count < audit_token.size())
        {
            return make_mig_return_code_reply(request, macos_kern_return::KERN_INVALID_ARGUMENT);
        }

        std::vector<std::byte> body(sizeof(int32_t) + sizeof(uint32_t) + sizeof(audit_token));
        const auto return_code = static_cast<int32_t>(macos_kern_return::KERN_SUCCESS);
        const auto count = static_cast<uint32_t>(audit_token.size());
        std::memcpy(body.data(), &return_code, sizeof(return_code));
        std::memcpy(body.data() + sizeof(return_code), &count, sizeof(count));
        std::memcpy(body.data() + sizeof(return_code) + sizeof(count), audit_token.data(), sizeof(audit_token));
        return make_mig_reply(request, body);
    }
}
