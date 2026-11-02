#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr size_t MACOS_CTL_MAXNAME = 12;
        constexpr int32_t MACOS_CTL_UNSPEC = 0;
        constexpr int32_t MACOS_CTL_UNSPEC_NAME2OID = 3;
        constexpr size_t MAX_NAME_LENGTH = 256;

        struct named_sysctl
        {
            std::string_view name;
            std::vector<int32_t> oid;
            std::string value;
        };

        // Real XNU OIDs, so callers that hard-code them (CTL_KERN = 1, KERN_VERSION = 4, ...) see the same entries.
        const std::vector<named_sysctl>& named_sysctls()
        {
            static const std::vector<named_sysctl> sysctls{
                {
                    .name = "kern.osversion",
                    .oid = {1, 65},
                    .value = std::string{"24H32"} + '\0',
                },
                {
                    .name = "kern.version",
                    .oid = {1, 4},
                    .value = std::string{"Darwin Kernel Version 24.6.0: Sun Aug 23 20:37:03 PDT 2026; "
                                         "root:xnu-11417.140.69.712.69~1/RELEASE_X86_64"} +
                             '\0',
                },
            };
            return sysctls;
        }

        void read_sysctl(const macos_syscall_context& c, const named_sysctl& entry, const uint64_t old_buffer,
                         const uint64_t old_length_pointer)
        {
            const uint64_t value_size = entry.value.size();
            if (old_length_pointer == 0)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            uint64_t old_length{};
            if (old_buffer != 0 && !c.emu.try_read_memory(old_length_pointer, &old_length, sizeof(old_length)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            if (old_buffer != 0)
            {
                if (old_length < value_size)
                {
                    write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
                    return;
                }

                if (!c.emu.try_write_memory(old_buffer, entry.value.data(), entry.value.size()))
                {
                    write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                    return;
                }
            }

            if (!c.emu.try_write_memory(old_length_pointer, &value_size, sizeof(value_size)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
        }

        void name_to_oid(const macos_syscall_context& c, const uint64_t old_buffer, const uint64_t old_length_pointer,
                         const uint64_t new_buffer, const size_t new_length)
        {
            if (new_length == 0 || new_length > MAX_NAME_LENGTH)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
                return;
            }

            std::string name(new_length, '\0');
            uint64_t old_length{};
            if (!c.emu.try_read_memory(new_buffer, name.data(), name.size()) ||
                !c.emu.try_read_memory(old_length_pointer, &old_length, sizeof(old_length)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            name.resize(name.find('\0') == std::string::npos ? name.size() : name.find('\0'));

            const auto& sysctls = named_sysctls();
            const auto it = std::ranges::find(sysctls, std::string_view{name}, &named_sysctl::name);
            c.emu_ref.log.log("sysctl name2oid(%s)%s\n", name.c_str(), it == sysctls.end() ? " -> ENOENT" : "");

            if (it == sysctls.end())
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
                return;
            }

            const auto oid_size = it->oid.size() * sizeof(int32_t);
            if (old_length < oid_size)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
                return;
            }

            const uint64_t written = oid_size;
            if (!c.emu.try_write_memory(old_buffer, it->oid.data(), oid_size) ||
                !c.emu.try_write_memory(old_length_pointer, &written, sizeof(written)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
        }

        std::string format_mib(const std::vector<int32_t>& mib)
        {
            std::string text{};
            for (const auto component : mib)
            {
                if (!text.empty())
                {
                    text += '.';
                }

                text += std::to_string(component);
            }

            return text;
        }
    }

    void sys_sysctl(const macos_syscall_context& c)
    {
        const auto name = get_macos_syscall_argument(c.emu, 0);
        const auto name_length = static_cast<size_t>(get_macos_syscall_argument(c.emu, 1));

        if (name_length == 0 || name_length > MACOS_CTL_MAXNAME)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        std::vector<int32_t> mib(name_length);
        if (!c.emu.try_read_memory(name, mib.data(), mib.size() * sizeof(int32_t)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (mib.size() == 2 && mib[0] == MACOS_CTL_UNSPEC && mib[1] == MACOS_CTL_UNSPEC_NAME2OID)
        {
            name_to_oid(c, get_macos_syscall_argument(c.emu, 2), get_macos_syscall_argument(c.emu, 3), get_macos_syscall_argument(c.emu, 4),
                        static_cast<size_t>(get_macos_syscall_argument(c.emu, 5)));
            return;
        }

        const auto& sysctls = named_sysctls();
        const auto entry = std::ranges::find(sysctls, mib, &named_sysctl::oid);
        if (entry != sysctls.end())
        {
            read_sysctl(c, *entry, get_macos_syscall_argument(c.emu, 2), get_macos_syscall_argument(c.emu, 3));
            return;
        }

        c.emu_ref.log.error("Unhandled sysctl %s\n", format_mib(mib).c_str());
        c.emu_ref.stop();
    }
}
