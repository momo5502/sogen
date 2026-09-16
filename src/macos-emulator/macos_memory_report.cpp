#include "std_include.hpp"
#include "macos_memory_report.hpp"

#include <platform/compiler.hpp>

#ifdef OS_EMSCRIPTEN
#include <emscripten/heap.h>
#elif defined(OS_MAC)
#include <mach/mach.h>
#elif defined(OS_LINUX)
#include <cstdio>
#include <unistd.h>
#endif

namespace sogen
{
    uint64_t query_host_heap_bytes()
    {
#ifdef OS_EMSCRIPTEN
        return emscripten_get_heap_size();
#elif defined(OS_MAC)
        mach_task_basic_info info{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        {
            return 0;
        }

        return info.resident_size;
#elif defined(OS_LINUX)
        auto* file = std::fopen("/proc/self/statm", "re");
        if (file == nullptr)
        {
            return 0;
        }

        unsigned long long total = 0;
        unsigned long long resident = 0;
        const auto scanned = std::fscanf(file, "%llu %llu", &total, &resident);
        (void)std::fclose(file);

        if (scanned != 2)
        {
            return 0;
        }

        return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
        return 0;
#endif
    }

    macos_memory_report collect_macos_memory_report(const macos_memory_manager& memory)
    {
        macos_memory_report report{};

        for (const auto& [address, region] : memory.get_mapped_regions())
        {
            ++report.guest_region_count;
            report.guest_reserved_bytes += region.length;

            if (region.backed)
            {
                report.guest_committed_bytes += region.length;
            }
        }

        report.host_heap_bytes = query_host_heap_bytes();
        return report;
    }

    std::string format_macos_memory_report(const macos_memory_report& report)
    {
        return "memory: committed=" + std::to_string(report.guest_committed_bytes) +
               " reserved=" + std::to_string(report.guest_reserved_bytes) + " regions=" + std::to_string(report.guest_region_count) +
               " host=" + std::to_string(report.host_heap_bytes) + " cache-resident=" + std::to_string(report.cache_resident_bytes);
    }
}
