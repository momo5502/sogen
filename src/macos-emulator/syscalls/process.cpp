#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MAIN_THREAD_ID = 0x1000;
        constexpr uint64_t PROCESS_UNIQUE_ID = 0x1000;
        constexpr uint32_t PARENT_PROCESS_ID = 1;
        constexpr uint64_t PARENT_UNIQUE_ID = 0x1;
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
        write_macos_syscall_success(c, MACOS_PROCESS_ID);
    }

    void sys_getuid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, USER_ID);
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
        constexpr uint64_t proc_info_call_pidinfo = 2;
        constexpr uint64_t proc_info_call_set_dyld_images = 15;
        constexpr uint64_t proc_pidt_shortbsdinfo = 13;
        constexpr uint64_t proc_pid_uniqidentifierinfo = 17;

        const auto call = get_macos_syscall_argument(c.emu, 0);
        const auto pid = get_macos_syscall_argument(c.emu, 1);
        const auto flavor = get_macos_syscall_argument(c.emu, 2);
        const auto buffer = get_macos_syscall_argument(c.emu, 4);
        const auto buffer_size = get_macos_syscall_argument(c.emu, 5);

        if (call == proc_info_call_pidinfo && flavor == proc_pidt_shortbsdinfo && pid == MACOS_PROCESS_ID)
        {
            struct proc_bsdshortinfo
            {
                uint32_t pid;
                uint32_t parent_pid;
                uint32_t process_group;
                uint32_t status;
                std::array<char, 16> command;
                uint32_t flags;
                std::array<uint32_t, 6> identities;
                uint32_t reserved;
            };

            static_assert(sizeof(proc_bsdshortinfo) == 64);

            // A native (not translated) 64-bit process that has exec'd, as measured on a shell-launched process.
            constexpr uint32_t process_flags = 0x1404010;
            constexpr uint32_t status_running = 2;

            proc_bsdshortinfo info{
                .pid = MACOS_PROCESS_ID,
                .parent_pid = PARENT_PROCESS_ID,
                .process_group = MACOS_PROCESS_ID,
                .status = status_running,
                .command = {},
                .flags = process_flags,
                .identities = {USER_ID, GROUP_ID, USER_ID, GROUP_ID, USER_ID, GROUP_ID},
                .reserved = 0,
            };

            const auto name = c.emu_ref.process_name;
            std::copy_n(name.begin(), std::min(name.size(), info.command.size() - 1), info.command.begin());

            if (buffer_size < sizeof(info) || !c.emu.try_write_memory(buffer, &info, sizeof(info)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
                return;
            }

            write_macos_syscall_success(c, sizeof(info));
            return;
        }

        if (call == proc_info_call_pidinfo && flavor == proc_pid_uniqidentifierinfo && pid == MACOS_PROCESS_ID)
        {
            struct proc_uniqidentifierinfo
            {
                std::array<uint8_t, 16> uuid;
                uint64_t unique_id;
                uint64_t parent_unique_id;
                int32_t id_version;
                uint32_t reserved2;
                uint64_t reserved3;
                uint64_t reserved4;
            };

            static_assert(sizeof(proc_uniqidentifierinfo) == 56);

            const proc_uniqidentifierinfo info{
                .uuid = c.emu_ref.executable_uuid,
                .unique_id = PROCESS_UNIQUE_ID,
                .parent_unique_id = PARENT_UNIQUE_ID,
                .id_version = static_cast<int32_t>(MACOS_PROCESS_ID_VERSION),
                .reserved2 = 0,
                .reserved3 = 0,
                .reserved4 = 0,
            };

            if (buffer_size < sizeof(info) || !c.emu.try_write_memory(buffer, &info, sizeof(info)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
                return;
            }

            write_macos_syscall_success(c, sizeof(info));
            return;
        }

        // dyld registers where its image list lives so debuggers can find it; nothing in the emulator reads it yet.
        if (call == proc_info_call_set_dyld_images)
        {
            write_macos_syscall_success(c, 0);
            return;
        }

        c.emu_ref.log.error("Unhandled proc_info(call %llu, pid %lld, flavor %llu)\n", static_cast<unsigned long long>(call),
                            static_cast<long long>(pid), static_cast<unsigned long long>(flavor));
        c.emu_ref.stop();
    }

    // Values observed from the kernel for a process launched from a shell: the supported pthread features, the
    // main thread's QoS, the default mutex policy, and Intel macOS' area for secondary thread stacks.
    void sys_bsdthread_register(const macos_syscall_context& c)
    {
        constexpr uint64_t supported_features = 0x400011DF;
        constexpr uint64_t main_thread_qos = 0x20FF;
        constexpr uint64_t thread_stack_hint = 0x700000000000;
        constexpr uint32_t mutex_default_policy = 0;

        constexpr uint64_t main_qos_offset = 16;
        constexpr uint64_t stack_addr_hint_offset = 36;
        constexpr uint64_t mutex_default_policy_offset = 44;

        const auto registration_data = get_macos_syscall_argument(c.emu, 3);
        const auto registration_data_size = get_macos_syscall_argument(c.emu, 4);

        if (registration_data != 0 && registration_data_size >= mutex_default_policy_offset + sizeof(mutex_default_policy))
        {
            if (!c.emu.try_write_memory(registration_data + main_qos_offset, &main_thread_qos, sizeof(main_thread_qos)) ||
                !c.emu.try_write_memory(registration_data + stack_addr_hint_offset, &thread_stack_hint, sizeof(thread_stack_hint)) ||
                !c.emu.try_write_memory(registration_data + mutex_default_policy_offset, &mutex_default_policy,
                                        sizeof(mutex_default_policy)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }
        }

        write_macos_syscall_success(c, supported_features);
    }

    namespace
    {
        // The status of an Apple platform binary as macOS reports it (measured on /bin/sleep, natively and under
        // Rosetta): valid, signed, platform binary, restricted. Its DER entitlements query succeeds without
        // writing anything, as XNU does when a binary has none.
        void code_signing_operation(const macos_syscall_context& c)
        {
            constexpr uint64_t cs_ops_status = 0;
            constexpr uint64_t cs_ops_der_entitlements_blob = 16;
            constexpr uint32_t platform_binary_status = 0x26010801;

            const auto pid = get_macos_syscall_argument(c.emu, 0);
            const auto operation = get_macos_syscall_argument(c.emu, 1);
            const auto buffer = get_macos_syscall_argument(c.emu, 2);
            const auto size = get_macos_syscall_argument(c.emu, 3);

            if (pid != 0 && pid != MACOS_PROCESS_ID)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ESRCH);
                return;
            }

            switch (operation)
            {
            case cs_ops_status:
                if (size < sizeof(platform_binary_status) ||
                    !c.emu.try_write_memory(buffer, &platform_binary_status, sizeof(platform_binary_status)))
                {
                    write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
                    return;
                }

                write_macos_syscall_success(c, 0);
                return;
            case cs_ops_der_entitlements_blob:
                write_macos_syscall_success(c, 0);
                return;
            default:
                c.emu_ref.log.error("Unhandled csops(pid %lld, op %llu, size %llu)\n", static_cast<long long>(pid),
                                    static_cast<unsigned long long>(operation), static_cast<unsigned long long>(size));
                c.emu_ref.stop();
                return;
            }
        }
    }

    void sys_csops(const macos_syscall_context& c)
    {
        code_signing_operation(c);
    }

    // XNU first checks that the audit token names this very process (pid and pid version).
    void sys_csops_audittoken(const macos_syscall_context& c)
    {
        std::array<uint32_t, 8> token{};
        if (!c.emu.try_read_memory(get_macos_syscall_argument(c.emu, 4), token.data(), sizeof(token)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (token[5] != MACOS_PROCESS_ID || token[7] != MACOS_PROCESS_ID_VERSION)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ESRCH);
            return;
        }

        code_signing_operation(c);
    }

    void sys_issetugid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, 0);
    }

    void sys_getentropy(const macos_syscall_context& c)
    {
        constexpr uint64_t max_entropy_size = 256;

        const auto buffer = get_macos_syscall_argument(c.emu, 0);
        const auto size = get_macos_syscall_argument(c.emu, 1);
        if (size > max_entropy_size)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        std::vector<std::byte> data(static_cast<size_t>(size));
        for (size_t offset = 0; offset < data.size(); offset += sizeof(uint64_t))
        {
            const auto value = c.emu_ref.next_entropy();
            std::memcpy(data.data() + offset, &value, std::min(sizeof(value), data.size() - offset));
        }

        if (!c.emu.try_write_memory(buffer, data.data(), data.size()))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}
