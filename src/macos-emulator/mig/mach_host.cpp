#include "macos_mig.hpp"
#include "../macho.hpp"
#include "../macos_commpage.hpp"

#include <algorithm>
#include <array>

namespace sogen
{
    namespace
    {
        constexpr int32_t HOST_BASIC_INFO = 1;

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

        std::vector<std::byte> body{};
        if (flavor != HOST_BASIC_INFO)
        {
            append(body, static_cast<int32_t>(macos_kern_return::KERN_INVALID_ARGUMENT));
            return make_mig_reply(request, body);
        }

        const host_basic_info info{
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

        std::array<std::byte, sizeof(info)> info_bytes{};
        std::memcpy(info_bytes.data(), &info, sizeof(info));

        const auto count = std::min<uint32_t>(requested_count, sizeof(info) / sizeof(int32_t));

        append(body, static_cast<int32_t>(macos_kern_return::KERN_SUCCESS));
        append(body, count);
        body.insert(body.end(), info_bytes.begin(), info_bytes.begin() + (count * sizeof(int32_t)));

        return make_mig_reply(request, body);
    }
}
