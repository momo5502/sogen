#include "host_watchpoints.hpp"

#if defined(__APPLE__) && defined(__aarch64__)
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <csignal>

#include <mach/mach.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace sogen::utils::host_watchpoints
{
#if defined(__APPLE__) && defined(__aarch64__)
    namespace
    {
        // Arm ARM, DBGWCR<n>_EL1.
        constexpr uint64_t control_enable = 1;
        constexpr uint64_t control_el0 = 2ULL << 1;
        constexpr uint64_t control_loads_and_stores = 3ULL << 3;
        constexpr uint64_t control_all_bytes = 0xFFULL << 5;
        constexpr int control_mask_shift = 24;

        constexpr size_t debug_state_slots = std::size(arm_debug_state64_t{}.__wvr);

        uint64_t control_for(const range& watched)
        {
            return control_enable | control_el0 | control_loads_and_stores | control_all_bytes |
                   (static_cast<uint64_t>(std::countr_zero(watched.size)) << control_mask_shift);
        }

        bool set_thread_watchpoints(const std::span<const range> ranges)
        {
            arm_debug_state64_t state{};
            for (size_t i = 0; i < ranges.size(); ++i)
            {
                state.__wvr[i] = ranges[i].address;
                state.__wcr[i] = control_for(ranges[i]);
            }

            return ::thread_set_state(::pthread_mach_thread_np(::pthread_self()), ARM_DEBUG_STATE64,
                                      reinterpret_cast<thread_state_t>(&state), ARM_DEBUG_STATE64_COUNT) == KERN_SUCCESS;
        }

        size_t reported_watchpoint_count()
        {
            int count = 0;
            size_t size = sizeof(count);
            if (::sysctlbyname("hw.optional.watchpoint", &count, &size, nullptr, 0) != 0 || count <= 0)
            {
                return 0;
            }

            return std::min(static_cast<size_t>(count), debug_state_slots);
        }

        // A debugger receives the watchpoint exception through its exception port, so the process gets no
        // signal for it (Apple Technical Q&A QA1361 for the check).
        bool is_being_debugged()
        {
            kinfo_proc info{};
            size_t size = sizeof(info);
            std::array<int, 4> name{CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
            return ::sysctl(name.data(), static_cast<u_int>(name.size()), &info, &size, nullptr, 0) == 0 &&
                   (info.kp_proc.p_flag & P_TRACED) != 0;
        }

        std::atomic<int> g_probe_hits{0};

        void record_probe_hit(int, siginfo_t*, void*)
        {
            ++g_probe_hits;
            set_thread_watchpoints({});
        }

        bool read_hits(const range& watched, const uint64_t address)
        {
            const int hits_before = g_probe_hits;
            if (!set_thread_watchpoints({&watched, 1}))
            {
                return false;
            }

            (void)*reinterpret_cast<const volatile uint8_t*>(address);
            set_thread_watchpoints({});
            return g_probe_hits != hits_before;
        }

        // Proves the form the callers rely on: a whole 4KB page watched through MASK, with the hit
        // delivered as a signal. A byte past the first eight would not hit if MASK were ignored, and the
        // byte after the page must not hit.
        bool page_watchpoints_work()
        {
            constexpr size_t probe_size = 0x4000;
            constexpr size_t watched_size = 0x1000;

            void* const mapping = ::mmap(nullptr, probe_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (mapping == MAP_FAILED)
            {
                return false;
            }

            struct sigaction probe{};
            probe.sa_sigaction = record_probe_hit;
            probe.sa_flags = SA_SIGINFO;
            sigemptyset(&probe.sa_mask);

            struct sigaction previous{};
            ::sigaction(SIGTRAP, &probe, &previous);

            const auto page = reinterpret_cast<uint64_t>(mapping);
            const range watched{.address = page, .size = watched_size};
            const bool works = read_hits(watched, page + watched_size - 8) && !read_hits(watched, page + watched_size);

            ::sigaction(SIGTRAP, &previous, nullptr);
            ::munmap(mapping, probe_size);

            return works;
        }
    }

    size_t capacity()
    {
        static const size_t count = [] {
            const auto reported = reported_watchpoint_count();
            return reported != 0 && !is_being_debugged() && page_watchpoints_work() ? reported : 0;
        }();

        return count;
    }

    bool available()
    {
        return capacity() != 0 && !is_being_debugged();
    }

    bool program(const std::span<const range> ranges)
    {
        return ranges.size() <= capacity() && set_thread_watchpoints(ranges);
    }
#else
    size_t capacity()
    {
        return 0;
    }

    bool available()
    {
        return false;
    }

    bool program(const std::span<const range> ranges)
    {
        return ranges.empty();
    }
#endif
}
