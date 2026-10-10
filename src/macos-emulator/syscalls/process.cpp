#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

namespace sogen
{
    namespace
    {
        // Not 1: dyld and libSystem take launchd-only paths for pid 1.
        constexpr uint64_t PROCESS_ID = 100;
        constexpr uint64_t MAIN_THREAD_ID = 0x1000;
    }

    void sys_exit(const macos_syscall_context& c)
    {
        c.emu_ref.exit_status = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        c.emu_ref.stop();
    }

    void sys_thread_selfid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, MAIN_THREAD_ID);
    }

    void sys_getpid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, PROCESS_ID);
    }

    // Two policy calls are answered. AMFI's dyld policy query takes { uint64_t input_flags; uint64_t* output_flags }
    // and dyld copies the output even when the call fails, so failing would hand it uninitialized (permissive)
    // flags; zero is the policy of a restricted platform binary. The sandbox check's arguments start with a
    // pointer to its result; the process is not sandboxed, so every check is allowed (zero).
    void sys_mac_syscall(const macos_syscall_context& c)
    {
        constexpr uint64_t amfi_check_dyld_policy_self = 0x5A;
        constexpr uint64_t sandbox_check = 2;

        const auto policy = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        const auto call = get_macos_syscall_argument(c.emu, 1);
        const auto argument = get_macos_syscall_argument(c.emu, 2);

        uint64_t output_offset{};
        if (policy == "AMFI" && call == amfi_check_dyld_policy_self)
        {
            output_offset = sizeof(uint64_t);
        }
        else if (policy == "Sandbox" && call == sandbox_check)
        {
            output_offset = 0;
        }
        else
        {
            c.emu_ref.log.error("Unhandled __mac_syscall(%s, 0x%llx)\n", policy ? policy->c_str() : "?",
                                static_cast<unsigned long long>(call));
            c.emu_ref.stop();
            return;
        }

        uint64_t output_pointer{};
        constexpr uint64_t output_value = 0;
        if (!c.emu.try_read_memory(argument + output_offset, &output_pointer, sizeof(output_pointer)) ||
            !c.emu.try_write_memory(output_pointer, &output_value, sizeof(output_value)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 0);
    }

    // System Integrity Protection is reported as fully enabled, as on a stock Mac: every protection-disabling
    // flag is denied and the active configuration is empty.
    void sys_csrctl(const macos_syscall_context& c)
    {
        constexpr uint64_t csr_syscall_check = 0;
        constexpr uint64_t csr_syscall_get_active_config = 1;

        const auto operation = get_macos_syscall_argument(c.emu, 0);
        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto size = get_macos_syscall_argument(c.emu, 2);

        if (operation == csr_syscall_check)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EPERM);
            return;
        }

        if (operation == csr_syscall_get_active_config)
        {
            constexpr uint32_t active_config = 0;
            if (size != sizeof(active_config) || !c.emu.try_write_memory(buffer, &active_config, sizeof(active_config)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
                return;
            }

            write_macos_syscall_success(c, 0);
            return;
        }

        write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
    }

    void sys_abort_with_payload(const macos_syscall_context& c)
    {
        const auto reason_namespace = get_macos_syscall_argument(c.emu, 0);
        const auto reason_code = get_macos_syscall_argument(c.emu, 1);
        const auto reason = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 4));

        c.emu_ref.log.error("Process aborted (namespace %llu, code 0x%llx): %s\n", static_cast<unsigned long long>(reason_namespace),
                            static_cast<unsigned long long>(reason_code), reason ? reason->c_str() : "?");
        c.emu_ref.stop();
    }

    void sys_proc_info(const macos_syscall_context& c)
    {
        constexpr uint64_t proc_info_call_set_dyld_images = 15;

        const auto call = get_macos_syscall_argument(c.emu, 0);

        // dyld registers where its image list lives so debuggers can find it; nothing in the emulator reads it yet.
        if (call == proc_info_call_set_dyld_images)
        {
            write_macos_syscall_success(c, 0);
            return;
        }

        c.emu_ref.log.error("Unhandled proc_info(call %llu, pid %lld, flavor %llu)\n", static_cast<unsigned long long>(call),
                            static_cast<long long>(get_macos_syscall_argument(c.emu, 1)),
                            static_cast<unsigned long long>(get_macos_syscall_argument(c.emu, 2)));
        c.emu_ref.stop();
    }
}
