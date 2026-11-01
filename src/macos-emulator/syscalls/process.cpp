#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <utils/io.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MAIN_THREAD_ID = 0x1000;
        constexpr size_t MACOS_ARG_MAX = 1024 * 1024;
        constexpr size_t MACOS_MAXPATHLEN = 1024;

        // execve's argv and envp: a NULL-terminated array of string pointers, where a NULL array is empty. Together they
        // must fit in ARG_MAX bytes, terminators included.
        uint64_t read_exec_strings(x86_64_emulator& emu, const uint64_t array, size_t& remaining_size, std::vector<std::string>& strings)
        {
            if (array == 0)
            {
                return 0;
            }

            for (uint64_t index = 0;; ++index)
            {
                uint64_t address{};
                if (!emu.try_read_memory(array + (index * sizeof(address)), &address, sizeof(address)))
                {
                    return macos_errno::MACOS_EFAULT;
                }

                if (address == 0)
                {
                    return 0;
                }

                auto& string = strings.emplace_back();
                while (true)
                {
                    char character{};
                    if (!emu.try_read_memory(address + string.size(), &character, sizeof(character)))
                    {
                        return macos_errno::MACOS_EFAULT;
                    }

                    if (remaining_size == 0)
                    {
                        return macos_errno::MACOS_E2BIG;
                    }

                    --remaining_size;
                    if (character == '\0')
                    {
                        break;
                    }

                    string.push_back(character);
                }
            }
        }

        uint64_t check_executable_file(const std::filesystem::path& host_path)
        {
            constexpr auto any_execute =
                std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;

            std::error_code error{};
            const auto status = std::filesystem::status(host_path, error);
            if (!std::filesystem::exists(status))
            {
                return macos_errno::MACOS_ENOENT;
            }

            if (!std::filesystem::is_regular_file(status) || (status.permissions() & any_execute) == std::filesystem::perms::none)
            {
                return macos_errno::MACOS_EACCES;
            }

            return 0;
        }
    }

    void sys_exit(const macos_syscall_context& c)
    {
        c.emu_ref.exit_status = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        c.emu_ref.stop();
    }

    void sys_fork(const macos_syscall_context& c)
    {
        auto& emu = c.emu_ref;
        emu.after_syscall([&emu] {
            const auto child_id = emu.fork_process([](macos_emulator& child) {
                const macos_syscall_context child_context{.emu_ref = child, .emu = child.emu()};
                write_macos_syscall_success(child_context, child.identity.process_id);
                child.emu().reg(x86_register::rdx, uint64_t{1});
            });

            write_macos_syscall_success({.emu_ref = emu, .emu = emu.emu()}, child_id);
            emu.emu().reg(x86_register::rdx, uint64_t{0});
        });
    }

    void sys_execve(const macos_syscall_context& c)
    {
        const auto path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0), MACOS_MAXPATHLEN);
        if (!path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (path->empty())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        std::vector<std::string> argv{};
        std::vector<std::string> envp{};
        size_t remaining_size = MACOS_ARG_MAX;
        for (const auto& [array, strings] :
             {std::pair{get_macos_syscall_argument(c.emu, 1), &argv}, std::pair{get_macos_syscall_argument(c.emu, 2), &envp}})
        {
            if (const auto error = read_exec_strings(c.emu, array, remaining_size, *strings))
            {
                write_macos_syscall_error(c, error);
                return;
            }
        }

        const auto host_path = c.emu_ref.file_system.translate(c.emu_ref.resolve_guest_path(*path));
        if (const auto error = check_executable_file(host_path))
        {
            c.emu_ref.log.log("execve(%s) -> %llu\n", path->c_str(), static_cast<unsigned long long>(error));
            write_macos_syscall_error(c, error);
            return;
        }

        std::vector<std::byte> image{};
        if (!utils::io::read_file(host_path, &image))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EIO);
            return;
        }

        if (!is_macos_executable(image))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOEXEC);
            return;
        }

        c.emu_ref.log.log("execve(%s)\n", path->c_str());

        auto& emu = c.emu_ref;
        emu.after_syscall([&emu, image = std::move(image), path = *path, argv = std::move(argv), envp = std::move(envp)] {
            emu.exec_image(image, path, argv, envp);
        });
    }

    // Children have always terminated by the time their parent runs, so waiting never blocks.
    void sys_wait4(const macos_syscall_context& c)
    {
        constexpr size_t rusage_size = 144;

        const auto selector = static_cast<int32_t>(get_macos_syscall_argument(c.emu, 0));
        const auto status_address = get_macos_syscall_argument(c.emu, 1);
        const auto rusage_address = get_macos_syscall_argument(c.emu, 3);

        const auto child = c.emu_ref.children.find(selector, c.emu_ref.identity.process_group_id);
        if (!child)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ECHILD);
            return;
        }

        constexpr std::array<std::byte, rusage_size> rusage{};
        if ((rusage_address != 0 && !c.emu.try_write_memory(rusage_address, rusage.data(), rusage.size())) ||
            (status_address != 0 && !c.emu.try_write_memory(status_address, &child->wait_status, sizeof(child->wait_status))))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        c.emu_ref.children.reap(child->process_id);
        write_macos_syscall_success(c, child->process_id);
    }

    void sys_thread_selfid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, MAIN_THREAD_ID);
    }

    void sys_getpid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, c.emu_ref.identity.process_id);
    }

    void sys_getgroups(const macos_syscall_context& c)
    {
        constexpr std::array groups{GROUP_ID};

        const auto size = static_cast<uint32_t>(get_macos_syscall_argument(c.emu, 0));
        if (size == 0)
        {
            write_macos_syscall_success(c, groups.size());
            return;
        }

        if (size < groups.size())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        if (!c.emu.try_write_memory(get_macos_syscall_argument(c.emu, 1), groups.data(), sizeof(groups)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, groups.size());
    }

    void sys_getpgrp(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, c.emu_ref.identity.process_group_id);
    }

    // Only the calling process can join its own group: children have terminated by the time the parent runs again.
    void sys_setpgid(const macos_syscall_context& c)
    {
        const auto pid = static_cast<int32_t>(get_macos_syscall_argument(c.emu, 0));
        const auto group = static_cast<int32_t>(get_macos_syscall_argument(c.emu, 1));

        if (group < 0)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto& identity = c.emu_ref.identity;
        if (pid != 0 && pid != static_cast<int32_t>(identity.process_id))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ESRCH);
            return;
        }

        if (group != 0 && group != static_cast<int32_t>(identity.process_group_id))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EPERM);
            return;
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_getppid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, c.emu_ref.identity.parent_process_id);
    }

    void sys_getuid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, USER_ID);
    }

    void sys_geteuid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, USER_ID);
    }

    void sys_getgid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, GROUP_ID);
    }

    void sys_getegid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, GROUP_ID);
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

        const auto& identity = c.emu_ref.identity;

        if (call == proc_info_call_pidinfo && flavor == proc_pidt_shortbsdinfo && pid == identity.process_id)
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
                .pid = identity.process_id,
                .parent_pid = identity.parent_process_id,
                .process_group = identity.process_group_id,
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

        if (call == proc_info_call_pidinfo && flavor == proc_pid_uniqidentifierinfo && pid == identity.process_id)
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
                .unique_id = identity.unique_id,
                .parent_unique_id = identity.parent_unique_id,
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

            if (pid != 0 && pid != c.emu_ref.identity.process_id)
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

        if (token[5] != c.emu_ref.identity.process_id || token[7] != MACOS_PROCESS_ID_VERSION)
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

    // The limits of a process launched from a shell on an Intel Mac (ulimit -a, soft and hard).
    void sys_getrlimit(const macos_syscall_context& c)
    {
        struct resource_limit
        {
            uint64_t current;
            uint64_t maximum;
        };

        constexpr uint64_t posix_flag = 0x1000;
        constexpr uint64_t unlimited = 0x7FFFFFFFFFFFFFFF;
        constexpr std::array<resource_limit, 9> limits{{
            {.current = unlimited, .maximum = unlimited},
            {.current = unlimited, .maximum = unlimited},
            {.current = unlimited, .maximum = unlimited},
            {.current = 8 * 1024 * 1024, .maximum = 65532 * 1024},
            {.current = 0, .maximum = unlimited},
            {.current = unlimited, .maximum = unlimited},
            {.current = unlimited, .maximum = unlimited},
            {.current = 2666, .maximum = 2666},
            {.current = MACOS_DESCRIPTOR_LIMIT, .maximum = unlimited},
        }};

        const auto resource = get_macos_syscall_argument(c.emu, 0) & ~posix_flag;
        if (resource >= limits.size())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto& limit = limits.at(static_cast<size_t>(resource));
        if (!c.emu.try_write_memory(get_macos_syscall_argument(c.emu, 1), &limit, sizeof(limit)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}
