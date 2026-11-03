#include "macos_mig.hpp"
#include "../macho.hpp"
#include "../macos_commpage.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <cstddef>
#include <array>

namespace sogen
{
    namespace
    {
        constexpr int32_t HOST_BASIC_INFO = 1;
        constexpr int32_t HOST_PRIORITY_INFO = 5;

        // XNU's scheduler bands (osfmk/kern/sched.h): kernel and system at MINPRI_KERNEL, servers at
        // MINPRI_RESERVED, users at BASEPRI_DEFAULT, up to MAXPRI_RESERVED.
        constexpr std::array<int32_t, 8> HOST_PRIORITIES{80, 80, 64, 31, 0, 0, 0, 79};

        struct host_basic_info
        {
            int32_t max_cpus;
            int32_t avail_cpus;
            uint32_t memory_size;
            int32_t cpu_type;
            int32_t cpu_subtype;
            int32_t cpu_threadtype;
            int32_t physical_cpu;
            int32_t physical_cpu_max;
            int32_t logical_cpu;
            int32_t logical_cpu_max;
            uint64_t max_mem;
        };

        static_assert(sizeof(host_basic_info) == 12 * sizeof(int32_t));

        // CPU_SUBTYPE_X86_64_ALL rather than X86_64_H: the latter makes dyld prefer Haswell (AVX) slices and
        // variants, which the emulated CPU does not advertise.
        constexpr int32_t CPU_SUBTYPE_X86_64_ALL = 3;

        // XNU caps the 32-bit memory_size field at 2 GiB.
        constexpr uint64_t MEMORY_SIZE_FIELD_LIMIT = 0x80000000;

        template <typename T>
        void append(std::vector<std::byte>& data, const T& value)
        {
            const auto* bytes = reinterpret_cast<const std::byte*>(&value);
            data.insert(data.end(), bytes, bytes + sizeof(value));
        }
    }

    std::vector<std::byte> mig_host_info(const macos_syscall_context&, const std::span<const std::byte> request)
    {
        const auto flavor = read_mig_field<int32_t>(request, MIG_BODY_OFFSET);
        const auto requested_count = read_mig_field<uint32_t>(request, MIG_BODY_OFFSET + sizeof(int32_t));

        std::vector<std::byte> info{};
        if (flavor == HOST_BASIC_INFO)
        {
            const host_basic_info basic{
                .max_cpus = MACOS_CPU_COUNT,
                .avail_cpus = MACOS_CPU_COUNT,
                .memory_size = static_cast<uint32_t>(std::min(MACOS_MEMORY_SIZE, MEMORY_SIZE_FIELD_LIMIT)),
                .cpu_type = static_cast<int32_t>(macho::CPU_TYPE_X86_64),
                .cpu_subtype = CPU_SUBTYPE_X86_64_ALL,
                .cpu_threadtype = 0,
                .physical_cpu = MACOS_CPU_COUNT,
                .physical_cpu_max = MACOS_CPU_COUNT,
                .logical_cpu = MACOS_CPU_COUNT,
                .logical_cpu_max = MACOS_CPU_COUNT,
                .max_mem = MACOS_MEMORY_SIZE,
            };
            append(info, basic);
        }
        else if (flavor == HOST_PRIORITY_INFO)
        {
            append(info, HOST_PRIORITIES);
        }

        std::vector<std::byte> body{};
        const auto full_count = static_cast<uint32_t>(info.size() / sizeof(int32_t));
        if (info.empty() || (flavor == HOST_PRIORITY_INFO && requested_count < full_count))
        {
            append(body, static_cast<int32_t>(info.empty() ? macos_kern_return::KERN_INVALID_ARGUMENT : macos_kern_return::KERN_FAILURE));
            return make_mig_reply(request, body);
        }

        const auto count = std::min(requested_count, full_count);

        append(body, static_cast<int32_t>(macos_kern_return::KERN_SUCCESS));
        append(body, count);
        body.insert(body.end(), info.begin(), info.begin() + static_cast<std::ptrdiff_t>(count * sizeof(int32_t)));

        return make_mig_reply(request, body);
    }

    std::vector<std::byte> mig_host_get_clock_service(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        constexpr int32_t system_clock = 0;
        constexpr int32_t calendar_clock = 1;

        const auto clock_id = read_mig_field<int32_t>(request, MIG_BODY_OFFSET);
        if (clock_id != system_clock && clock_id != calendar_clock)
        {
            std::vector<std::byte> body{};
            append(body, static_cast<int32_t>(macos_kern_return::KERN_INVALID_ARGUMENT));
            return make_mig_reply(request, body);
        }

        const auto port = c.emu_ref.mach_ports.kernel_object_send_right(mach_port_object::clock, static_cast<uint32_t>(clock_id));
        return make_mig_port_reply(request, port);
    }

    std::vector<std::byte> mig_host_get_special_port(const macos_syscall_context& c, const std::span<const std::byte> request)
    {
        constexpr int32_t host_port = 1;

        const auto which = read_mig_field<int32_t>(request, MIG_BODY_OFFSET + sizeof(int32_t));
        if (which != host_port)
        {
            c.emu_ref.log.error("Unhandled host_get_special_port(%d)\n", which);
            return {};
        }

        return make_mig_port_reply(request, macos_mach_ports::HOST_PORT_NAME);
    }
}
