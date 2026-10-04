#include "../std_include.hpp"
#include "../macos_emulator.hpp"
#include "../macos_syscall_utils.hpp"

#include "sysctl_table.hpp"

#include <array>

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_errno;
    using namespace macos_sysctl_mib;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        constexpr uint64_t MACOS_SYSCTL_NAME_MAX = 256;
        constexpr uint32_t MACOS_SYSCTL_CTL_MAXNAME = 12;
        constexpr uint64_t MACOS_SYSCTL_ARGMAX = 1048576;
        constexpr uint64_t MACOS_SYSCTL_LITTLE_ENDIAN = 1234;

        macos_sysctl_value integer32_value(const uint64_t number)
        {
            return {.value_kind = macos_sysctl_value::kind::integer32, .number = number, .text = {}};
        }

        macos_sysctl_value integer64_value(const uint64_t number)
        {
            return {.value_kind = macos_sysctl_value::kind::integer64, .number = number, .text = {}};
        }

        macos_sysctl_value string_value(std::string text)
        {
            return {.value_kind = macos_sysctl_value::kind::string, .number = 0, .text = std::move(text)};
        }

        uint64_t user_stack_top(const macos_process_context& proc)
        {
            if (proc.stack_size == 0 || proc.stack_base > UINT64_MAX - proc.stack_size)
            {
                return MACOS_MAIN_STACK_TOP;
            }

            return proc.stack_base + proc.stack_size;
        }

        std::optional<macos_sysctl_value> resolve_hardware(const std::string_view name, const macos_system_info& info)
        {
            if (name == "hw.ncpu")
            {
                return integer32_value(info.ncpus);
            }

            if (name == "hw.activecpu" || name == "hw.availcpu")
            {
                return integer32_value(info.active_cpus);
            }

            if (name == "hw.physicalcpu")
            {
                return integer32_value(info.physical_cpus);
            }

            if (name == "hw.logicalcpu")
            {
                return integer32_value(info.logical_cpus);
            }

            if (name == "hw.memsize")
            {
                return integer64_value(info.memory_size);
            }

            if (name == "hw.pagesize" || name == "hw.pagesize32")
            {
                return integer32_value(MACOS_PAGE_SIZE);
            }

            if (name == "hw.cachelinesize")
            {
                return integer64_value(info.cache_linesize);
            }

            if (name == "hw.cpufamily")
            {
                return integer32_value(info.cpufamily);
            }

            if (name == "hw.byteorder")
            {
                return integer32_value(MACOS_SYSCTL_LITTLE_ENDIAN);
            }

            if (name == "hw.machine")
            {
                return string_value(info.machine);
            }

            if (name == "hw.model")
            {
                return string_value(info.model);
            }

            return std::nullopt;
        }

        std::optional<macos_sysctl_value> resolve_optional_feature(const std::string_view name, const macos_system_info& info)
        {
            if (!name.starts_with("hw.optional."))
            {
                return std::nullopt;
            }

            if (name == "hw.optional.arm64" || name == "hw.optional.floatingpoint" || name == "hw.optional.neon")
            {
                return integer32_value(1);
            }

            if (name == "hw.optional.arm.FEAT_PAuth")
            {
                return integer32_value(info.has_feat_pauth ? 1 : 0);
            }

            if (name == "hw.optional.arm.FEAT_LSE")
            {
                return integer32_value(info.has_feat_lse ? 1 : 0);
            }

            if (name == "hw.optional.arm.FEAT_FP16")
            {
                return integer32_value(info.has_feat_fp16 ? 1 : 0);
            }

            if (name == "hw.optional.arm.FEAT_BTI")
            {
                return integer32_value(info.has_feat_bti ? 1 : 0);
            }

            // Darwin answers an optional feature it does not carry with a zero rather than ENOENT, so a
            // guest probing for something the emulator does not model gets a clean "no".
            return integer32_value(0);
        }

        std::optional<macos_sysctl_value> resolve_kernel(const std::string_view name, const macos_system_info& info,
                                                         const macos_process_context& proc)
        {
            if (name == "kern.ostype")
            {
                return string_value(info.os_type);
            }

            if (name == "kern.osrelease")
            {
                return string_value(info.os_release);
            }

            if (name == "kern.osversion")
            {
                return string_value(info.os_version);
            }

            if (name == "kern.osproductversion")
            {
                return string_value(info.os_product_version);
            }

            if (name == "kern.osrevision")
            {
                return integer32_value(static_cast<uint32_t>(info.os_revision));
            }

            if (name == "kern.version")
            {
                return string_value("Darwin Kernel Version " + info.os_release);
            }

            if (name == "kern.hostname")
            {
                return string_value("sogen");
            }

            // Empty, which is what a Mac booted normally reports. dyld reads it looking for boot-args that
            // would change how it behaves, and an absent node is not the same answer as an empty one --
            // the first is a kernel that does not have the concept.
            // Seen as ENOENT in a real run. Each is answered as a constant describing the emulated
            // machine, not by asking the host: no boot arguments, no OS variant flags, no iOS app
            // support, no optional CPU capabilities beyond the ones named individually above, and
            // storage that is not ephemeral. Zero is the conservative direction for the capability
            // words -- a guest that believes a feature is absent does without it.
            if (name == "kern.osvariant_status")
            {
                return integer64_value(0);
            }

            if (name == "kern.iossupportversion")
            {
                return string_value("");
            }

            if (name == "hw.optional.arm.caps")
            {
                return integer64_value(0);
            }

            if (name == "hw.ephemeral_storage")
            {
                return integer32_value(0);
            }

            if (name == "kern.bootargs")
            {
                return string_value("");
            }

            // Lockdown Mode, off. libsystem asks during start-up and an absent node reads as a kernel too
            // old to have it, which sends the caller down a different path than the one a current macOS
            // takes.
            if (name == "security.mac.lockdown_mode_state")
            {
                return integer32_value(0);
            }

            if (name == "kern.argmax")
            {
                return integer32_value(MACOS_SYSCTL_ARGMAX);
            }

            if (name == "kern.usrstack64")
            {
                return integer64_value(user_stack_top(proc));
            }

            if (name == "kern.hv_support" || name == "kern.secure_kernel")
            {
                return integer32_value(0);
            }

            return std::nullopt;
        }

        size_t required_size(const macos_sysctl_value& value)
        {
            switch (value.value_kind)
            {
            case macos_sysctl_value::kind::integer32:
                return sizeof(uint32_t);
            case macos_sysctl_value::kind::integer64:
                return sizeof(uint64_t);
            case macos_sysctl_value::kind::string:
                return value.text.size() + 1;
            }

            return 0;
        }

        bool write_value(const macos_syscall_context& c, const uint64_t address, const macos_sysctl_value& value)
        {
            switch (value.value_kind)
            {
            case macos_sysctl_value::kind::integer32: {
                const auto narrowed = static_cast<uint32_t>(value.number);
                return c.emu_ref.memory.try_write_memory(address, &narrowed, sizeof(narrowed));
            }
            case macos_sysctl_value::kind::integer64:
                return c.emu_ref.memory.try_write_memory(address, &value.number, sizeof(value.number));
            case macos_sysctl_value::kind::string:
                return c.emu_ref.memory.try_write_memory(address, value.text.c_str(), value.text.size() + 1);
            }

            return false;
        }

        struct sysctl_request
        {
            uint64_t old_address{};
            uint64_t old_length_address{};
            uint64_t new_address{};
            uint64_t new_length{};
        };

        sysctl_request read_sysctl_request(const macos_syscall_context& c)
        {
            return {
                .old_address = get_macos_syscall_argument(c, 2),
                .old_length_address = get_macos_syscall_argument(c, 3),
                .new_address = get_macos_syscall_argument(c, 4),
                .new_length = get_macos_syscall_argument(c, 5),
            };
        }

        // kern.proc.pid, which CoreFoundation asks for while working out who the process is and treats a
        // failure as fatal -- __CFGetUGIDs aborts, and from outside that is indistinguishable from the
        // program aborting for its own reasons.
        //
        // kinfo_proc is 648 bytes of nested structs and unions. Every offset below was measured with
        // offsetof against the SDK's own headers rather than reconstructed from the field list, because a
        // single wrong offset produces a well-formed answer that is quietly untrue. Everything not named
        // here stays zero, which is what a field the emulator does not model should read as.
        constexpr size_t KINFO_PROC_SIZE = 648;
        constexpr size_t KP_PROC_P_STAT = 36;
        constexpr size_t KP_PROC_P_PID = 40;
        constexpr size_t KP_PROC_P_COMM = 243;
        constexpr size_t KP_PROC_P_COMM_CAPACITY = 17;
        constexpr size_t KP_EPROC_PCRED_P_RUID = 392;
        constexpr size_t KP_EPROC_PCRED_P_SVUID = 396;
        constexpr size_t KP_EPROC_PCRED_P_RGID = 400;
        constexpr size_t KP_EPROC_PCRED_P_SVGID = 404;
        constexpr size_t KP_EPROC_UCRED_CR_UID = 420;
        constexpr size_t KP_EPROC_UCRED_CR_NGROUPS = 424;
        constexpr size_t KP_EPROC_UCRED_CR_GROUPS = 428;
        constexpr size_t KP_EPROC_E_PPID = 560;
        constexpr size_t KP_EPROC_E_PGID = 564;

        constexpr uint8_t MACOS_SRUN = 2;

        std::vector<uint8_t> build_kinfo_proc(const macos_process_context& proc)
        {
            std::vector<uint8_t> record(KINFO_PROC_SIZE, 0);

            const auto put32 = [&](const size_t offset, const uint32_t value) {
                std::memcpy(record.data() + offset, &value, sizeof(value));
            };

            record[KP_PROC_P_STAT] = MACOS_SRUN;
            put32(KP_PROC_P_PID, proc.pid);
            put32(KP_EPROC_E_PPID, proc.ppid);
            put32(KP_EPROC_E_PGID, proc.pid);

            put32(KP_EPROC_UCRED_CR_UID, proc.euid);
            put32(KP_EPROC_UCRED_CR_NGROUPS, 1);
            put32(KP_EPROC_UCRED_CR_GROUPS, proc.egid);

            put32(KP_EPROC_PCRED_P_RUID, proc.uid);
            put32(KP_EPROC_PCRED_P_SVUID, proc.uid);
            put32(KP_EPROC_PCRED_P_RGID, proc.gid);
            put32(KP_EPROC_PCRED_P_SVGID, proc.gid);

            const auto name = std::filesystem::path{proc.executable_path}.filename().string();
            const auto count = std::min(name.size(), KP_PROC_P_COMM_CAPACITY - 1);
            std::memcpy(record.data() + KP_PROC_P_COMM, name.data(), count);

            return record;
        }

        // Answers the size query as well as the data one: every caller asks with a null buffer first to
        // learn how much to allocate, and reporting nothing there makes it allocate nothing.
        bool answer_kern_proc_pid(const macos_syscall_context& c, const std::span<const int32_t> mib, const sysctl_request& request)
        {
            constexpr int32_t CTL_KERN = 1;
            constexpr int32_t KERN_PROC = 14;
            constexpr int32_t KERN_PROC_PID = 1;

            if (mib.size() != 4 || mib[0] != CTL_KERN || mib[1] != KERN_PROC || mib[2] != KERN_PROC_PID)
            {
                return false;
            }

            if (static_cast<uint32_t>(mib[3]) != c.proc.pid)
            {
                write_macos_syscall_error(c, MACOS_ESRCH);
                return true;
            }

            uint64_t available = 0;
            if (request.old_length_address != 0 &&
                !c.emu_ref.memory.try_read_memory(request.old_length_address, &available, sizeof(available)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return true;
            }

            const auto record = build_kinfo_proc(c.proc);

            if (request.old_address != 0)
            {
                if (available < record.size())
                {
                    write_macos_syscall_error(c, MACOS_ENOMEM);
                    return true;
                }

                if (!c.emu_ref.memory.try_write_memory(request.old_address, record.data(), record.size()))
                {
                    write_macos_syscall_error(c, MACOS_EFAULT);
                    return true;
                }
            }

            const uint64_t written = record.size();
            if (request.old_length_address != 0 &&
                !c.emu_ref.memory.try_write_memory(request.old_length_address, &written, sizeof(written)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return true;
            }

            write_macos_syscall_result(c, 0);
            return true;
        }

        // sysctlnametomib() asks the kernel to translate a name into a numeric MIB, through the magic
        // {CTL_UNSPEC, 3} node. libsystem uses it before querying by number, and CoreFoundation reaches it
        // during start-up.
        //
        // The inverse is derived by searching the forward table rather than written out again: a second
        // copy would be one more thing to keep in step, and this one cannot disagree with the answer the
        // guest gets when it queries the MIB it was just handed.
        std::optional<std::array<int32_t, 2>> lookup_sysctl_mib(const std::string_view name)
        {
            constexpr int32_t roots[]{MACOS_CTL_KERN, MACOS_CTL_HW};
            constexpr int32_t MAX_LEAF = 256;

            for (const auto root : roots)
            {
                for (int32_t leaf = 0; leaf < MAX_LEAF; ++leaf)
                {
                    const std::array<int32_t, 2> candidate{root, leaf};
                    const auto resolved = macos_sysctl_mib_to_name(candidate);
                    if (resolved.has_value() && *resolved == name)
                    {
                        return candidate;
                    }
                }
            }

            // Names that have an answer but no static number. On a real kernel these are registered at
            // boot and given whatever oid is free, so a number of the emulator's own choosing is exactly
            // as meaningful -- a guest only ever learns it by asking here, and asks by number afterwards.
            for (size_t i = 0; i < MACOS_SYSCTL_DYNAMIC_NAMES.size(); ++i)
            {
                if (MACOS_SYSCTL_DYNAMIC_NAMES[i] == name)
                {
                    return std::array<int32_t, 2>{MACOS_CTL_DYNAMIC, static_cast<int32_t>(i)};
                }
            }

            return std::nullopt;
        }

        bool answer_name_to_oid(const macos_syscall_context& c, const std::span<const int32_t> mib, const sysctl_request& request)
        {
            constexpr int32_t CTL_UNSPEC = 0;
            constexpr int32_t NAME_TO_OID = 3;

            if (mib.size() != 2 || mib[0] != CTL_UNSPEC || mib[1] != NAME_TO_OID)
            {
                return false;
            }

            if (request.new_address == 0 || request.new_length == 0 || request.new_length > MACOS_SYSCTL_NAME_MAX)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return true;
            }

            std::string name(static_cast<size_t>(request.new_length), '\0');
            if (!c.emu_ref.memory.try_read_memory(request.new_address, name.data(), name.size()))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return true;
            }

            name.resize(std::strlen(name.c_str()));

            const auto found = lookup_sysctl_mib(name);
            if (!found)
            {
                // The same answer a real kernel gives for a name it does not have. A guest that asked is
                // prepared for it; inventing an oid would send the next query somewhere meaningless.
                //
                // Named anyway, because sysctlnametomib(3) leaves the caller's buffer untouched on
                // failure and callers that do not check go on to sysctl() the zeroed buffer at its full
                // CTL_MAXNAME length. That arrives here as the all-zero MIB, which says nothing about
                // what was actually being asked for; this line is the only place the name survives.
                c.emu_ref.log.warn("Unmapped Darwin sysctl name-to-oid: %s\n", name.c_str());
                write_macos_syscall_error(c, MACOS_ENOENT);
                return true;
            }

            uint64_t available = 0;
            if (request.old_length_address != 0 &&
                !c.emu_ref.memory.try_read_memory(request.old_length_address, &available, sizeof(available)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return true;
            }

            const uint64_t needed = found->size() * sizeof(int32_t);

            if (request.old_address != 0)
            {
                if (available < needed)
                {
                    write_macos_syscall_error(c, MACOS_ENOMEM);
                    return true;
                }

                if (!c.emu_ref.memory.try_write_memory(request.old_address, found->data(), needed))
                {
                    write_macos_syscall_error(c, MACOS_EFAULT);
                    return true;
                }
            }

            if (request.old_length_address != 0 && !c.emu_ref.memory.try_write_memory(request.old_length_address, &needed, sizeof(needed)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return true;
            }

            write_macos_syscall_result(c, 0);
            return true;
        }

        void answer_sysctl(const macos_syscall_context& c, const std::optional<macos_sysctl_value>& value, const sysctl_request& request)
        {
            if (request.new_address != 0 || request.new_length != 0)
            {
                write_macos_syscall_error(c, MACOS_EPERM);
                return;
            }

            if (!value.has_value())
            {
                write_macos_syscall_error(c, MACOS_ENOENT);
                return;
            }

            const auto needed = static_cast<uint64_t>(required_size(*value));

            if (request.old_address == 0)
            {
                if (request.old_length_address != 0 &&
                    !c.emu_ref.memory.try_write_memory(request.old_length_address, &needed, sizeof(needed)))
                {
                    write_macos_syscall_error(c, MACOS_EFAULT);
                    return;
                }

                write_macos_syscall_result(c, 0);
                return;
            }

            if (request.old_length_address == 0)
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            uint64_t available{};
            if (!c.emu_ref.memory.try_read_memory(request.old_length_address, &available, sizeof(available)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            if (available < needed)
            {
                if (!c.emu_ref.memory.try_write_memory(request.old_length_address, &needed, sizeof(needed)))
                {
                    write_macos_syscall_error(c, MACOS_EFAULT);
                    return;
                }

                write_macos_syscall_error(c, MACOS_ENOMEM);
                return;
            }

            if (!write_value(c, request.old_address, *value) ||
                !c.emu_ref.memory.try_write_memory(request.old_length_address, &needed, sizeof(needed)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            write_macos_syscall_result(c, 0);
        }

        std::string describe_mib(const std::span<const int32_t> mib)
        {
            std::string description{};

            for (const auto element : mib)
            {
                if (!description.empty())
                {
                    description.push_back('.');
                }

                description.append(std::to_string(element));
            }

            return description;
        }
    }

    std::optional<macos_sysctl_value> resolve_macos_sysctl(const std::string_view name, const macos_system_info& info,
                                                           const macos_process_context& proc)
    {
        if (auto hardware = resolve_hardware(name, info); hardware.has_value())
        {
            return hardware;
        }

        if (auto feature = resolve_optional_feature(name, info); feature.has_value())
        {
            return feature;
        }

        return resolve_kernel(name, info, proc);
    }

    std::optional<std::string> macos_sysctl_mib_to_name(const std::span<const int32_t> mib)
    {
        if (mib.size() != 2)
        {
            return std::nullopt;
        }

        // The other half of the dynamic registration above: a guest that asked for a number gets to use
        // it, and gets the same answer it would have got by name.
        if (mib[0] == MACOS_CTL_DYNAMIC)
        {
            const auto index = static_cast<size_t>(mib[1]);
            if (mib[1] >= 0 && index < MACOS_SYSCTL_DYNAMIC_NAMES.size())
            {
                return std::string{MACOS_SYSCTL_DYNAMIC_NAMES[index]};
            }

            return std::nullopt;
        }

        if (mib[0] == MACOS_CTL_HW)
        {
            switch (mib[1])
            {
            case MACOS_HW_MACHINE:
                return "hw.machine";
            case MACOS_HW_MODEL:
                return "hw.model";
            case MACOS_HW_NCPU:
                return "hw.ncpu";
            case MACOS_HW_BYTEORDER:
                return "hw.byteorder";
            case MACOS_HW_PAGESIZE:
                return "hw.pagesize";
            case MACOS_HW_CACHELINE:
                return "hw.cachelinesize";
            case MACOS_HW_MEMSIZE:
                return "hw.memsize";
            case MACOS_HW_AVAILCPU:
                return "hw.availcpu";
            default:
                return std::nullopt;
            }
        }

        if (mib[0] == MACOS_CTL_KERN)
        {
            switch (mib[1])
            {
            case MACOS_KERN_OSTYPE:
                return "kern.ostype";
            case MACOS_KERN_OSRELEASE:
                return "kern.osrelease";
            case MACOS_KERN_OSREV:
                return "kern.osrevision";
            case MACOS_KERN_VERSION:
                return "kern.version";
            case MACOS_KERN_ARGMAX:
                return "kern.argmax";
            case MACOS_KERN_HOSTNAME:
                return "kern.hostname";
            case MACOS_KERN_USRSTACK64:
                return "kern.usrstack64";
            case MACOS_KERN_OSVERSION:
                return "kern.osversion";
            default:
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    void sys_sysctl(const macos_syscall_context& c)
    {
        const auto mib_address = get_macos_syscall_argument(c, 0);
        const auto mib_length = static_cast<uint32_t>(get_macos_syscall_argument(c, 1));
        const auto request = read_sysctl_request(c);

        if (mib_length == 0 || mib_length > MACOS_SYSCTL_CTL_MAXNAME)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        std::array<int32_t, MACOS_SYSCTL_CTL_MAXNAME> mib{};
        if (!c.emu_ref.memory.try_read_memory(mib_address, mib.data(), mib_length * sizeof(int32_t)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        const std::span<const int32_t> requested{mib.data(), mib_length};

        if (answer_kern_proc_pid(c, requested, request) || answer_name_to_oid(c, requested, request))
        {
            return;
        }

        const auto name = macos_sysctl_mib_to_name(requested);
        if (!name.has_value())
        {
            const auto description = describe_mib(requested);
            c.emu_ref.log.warn("Unmapped Darwin sysctl MIB: %s\n", description.c_str());
            write_macos_syscall_error(c, MACOS_ENOENT);
            return;
        }

        answer_sysctl(c, resolve_macos_sysctl(*name, c.emu_ref.system_info, c.proc), request);
    }

    void sys_sysctlbyname(const macos_syscall_context& c)
    {
        const auto name_address = get_macos_syscall_argument(c, 0);
        const auto name_length = get_macos_syscall_argument(c, 1);
        const auto request = read_sysctl_request(c);

        if (name_length > MACOS_SYSCTL_NAME_MAX)
        {
            write_macos_syscall_error(c, MACOS_ENAMETOOLONG);
            return;
        }

        std::string name{};
        for (uint64_t i = 0; i < name_length; ++i)
        {
            char character{};
            if (!c.emu_ref.memory.try_read_memory(name_address + i, &character, sizeof(character)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            if (character == '\0')
            {
                break;
            }

            name.push_back(character);
        }

        auto value = resolve_macos_sysctl(name, c.emu_ref.system_info, c.proc);
        if (!value.has_value())
        {
            c.emu_ref.log.warn("Unmapped Darwin sysctl name: %s\n", name.c_str());
        }

        answer_sysctl(c, value, request);
    }

}
