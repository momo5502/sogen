// Multi-threaded stress sample for the multi-vCPU emulator (docs/multi-vcpu-design.md, Phase 4).
//
// Every workload below asserts a deterministic invariant that only holds when the emulator's
// cross-vCPU synchronization and memory are correct: broken mutual exclusion loses counter
// updates, a lost wakeup stalls a wait until it times out, and a memory-manager race corrupts a
// freshly written page. Run it at --vcpus 1..8; exit code 0 means every invariant held.
//
// The workload scale can be tuned with argv[1] (a positive multiplier, default 1) to trade run
// time for contention when iterating.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
    unsigned g_scale = 1;

    unsigned scaled(const unsigned base)
    {
        return base * g_scale;
    }

    // Number of guest worker threads. Fixed above any realistic --vcpus so guest threads always
    // outnumber vCPUs and genuinely contend, even at --vcpus 2.
    constexpr unsigned kThreads = 8;

    void run_threads(const unsigned count, const std::function<void(unsigned)>& body)
    {
        std::vector<std::thread> threads;
        threads.reserve(count);
        for (unsigned i = 0; i < count; ++i)
        {
            threads.emplace_back(body, i);
        }
        for (auto& t : threads)
        {
            t.join();
        }
    }

    // Workload 1 - mutual exclusion. Threads take a CRITICAL_SECTION and do a non-atomic
    // read-modify-write on a shared counter with a deliberately widened window between the read and
    // the write. If the emulator ever lets two vCPUs hold the section at once, updates are lost and
    // the final value falls short of the expected total.
    bool test_critical_section()
    {
        CRITICAL_SECTION cs;
        InitializeCriticalSection(&cs);

        volatile LONG guarded = 0;
        const unsigned iters = scaled(2000);

        run_threads(kThreads, [&](unsigned) {
            for (unsigned i = 0; i < iters; ++i)
            {
                EnterCriticalSection(&cs);
                const LONG value = guarded;
                // Widen the race window without yielding the vCPU (a syscall would serialize us). Simple
                // assignment rather than ++ because increment of a volatile is deprecated in C++20.
                for (volatile int spin = 0; spin < 64; spin = spin + 1)
                {
                }
                guarded = value + 1;
                LeaveCriticalSection(&cs);
            }
        });

        DeleteCriticalSection(&cs);

        const LONG expected = static_cast<LONG>(kThreads) * static_cast<LONG>(iters);
        if (guarded != expected)
        {
            printf("  critical section: lost updates, got %ld expected %ld\n", guarded, expected);
            return false;
        }
        return true;
    }

    // Workload 2 - interlocked atomics across vCPUs. On WHP these run on real cores, so the lock
    // prefix must stay atomic across host threads. A torn increment shows up as a short total.
    bool test_interlocked()
    {
        volatile LONG counter = 0;
        const unsigned iters = scaled(20000);

        run_threads(kThreads, [&](unsigned) {
            for (unsigned i = 0; i < iters; ++i)
            {
                InterlockedIncrement(&counter);
            }
        });

        const LONG expected = static_cast<LONG>(kThreads) * static_cast<LONG>(iters);
        if (counter != expected)
        {
            printf("  interlocked: torn increment, got %ld expected %ld\n", counter, expected);
            return false;
        }
        return true;
    }

    // Workload 3 - semaphore producer/consumer. Producers release exactly `total` permits; consumers
    // claim exactly `total` waits. Every wait must succeed - a lost wakeup surfaces as a timeout.
    bool test_semaphore()
    {
        const unsigned producers = kThreads / 2;
        const unsigned consumers = kThreads - producers;
        const unsigned per_producer = scaled(1000);
        const LONG total = static_cast<LONG>(producers) * static_cast<LONG>(per_producer);

        HANDLE sem = CreateSemaphoreW(nullptr, 0, total, nullptr);
        if (!sem)
        {
            puts("  semaphore: CreateSemaphore failed");
            return false;
        }

        std::atomic<LONG> claim{0};
        std::atomic<LONG> acquired{0};
        std::atomic<bool> ok{true};

        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        for (unsigned p = 0; p < producers; ++p)
        {
            threads.emplace_back([&] {
                for (unsigned i = 0; i < per_producer; ++i)
                {
                    ReleaseSemaphore(sem, 1, nullptr);
                }
            });
        }

        for (unsigned c = 0; c < consumers; ++c)
        {
            threads.emplace_back([&] {
                while (claim.fetch_add(1) < total)
                {
                    if (WaitForSingleObject(sem, 15000) != WAIT_OBJECT_0)
                    {
                        ok.store(false);
                        return;
                    }
                    acquired.fetch_add(1);
                }
            });
        }

        for (auto& t : threads)
        {
            t.join();
        }

        CloseHandle(sem);

        if (!ok.load())
        {
            puts("  semaphore: a consumer wait timed out (lost wakeup)");
            return false;
        }
        if (acquired.load() != total)
        {
            printf("  semaphore: consumed %ld expected %ld\n", acquired.load(), total);
            return false;
        }
        return true;
    }

    // Workload 4 - auto-reset event ping-pong. Two threads hand a token back and forth through a tight
    // set/wait loop. This is the densest wait/wake path; a single dropped signal deadlocks one side,
    // caught by the finite wait timeout.
    bool test_event_pingpong()
    {
        HANDLE e_a = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE e_b = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!e_a || !e_b)
        {
            puts("  ping-pong: CreateEvent failed");
            return false;
        }

        const unsigned rounds = scaled(1500);
        std::atomic<bool> ok{true};

        std::thread a([&] {
            for (unsigned i = 0; i < rounds; ++i)
            {
                SetEvent(e_a);
                if (WaitForSingleObject(e_b, 15000) != WAIT_OBJECT_0)
                {
                    ok.store(false);
                    return;
                }
            }
        });

        std::thread b([&] {
            for (unsigned i = 0; i < rounds; ++i)
            {
                if (WaitForSingleObject(e_a, 15000) != WAIT_OBJECT_0)
                {
                    ok.store(false);
                    return;
                }
                SetEvent(e_b);
            }
        });

        a.join();
        b.join();

        CloseHandle(e_a);
        CloseHandle(e_b);

        if (!ok.load())
        {
            puts("  ping-pong: a wait timed out (lost wakeup)");
            return false;
        }
        return true;
    }

    // Workload 5 - VirtualAlloc churn with write/read verification. Each thread repeatedly commits a
    // page, stamps it with a value unique to (thread, iteration), reads it back, and frees it. Under a
    // memory-manager race two threads could receive overlapping backing pages, so the readback would
    // not match what this thread wrote.
    bool test_memory_churn()
    {
        const unsigned iters = scaled(800);
        std::atomic<bool> ok{true};

        run_threads(kThreads, [&](unsigned tid) {
            for (unsigned i = 0; i < iters && ok.load(); ++i)
            {
                auto* page = static_cast<uint32_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (!page)
                {
                    ok.store(false);
                    return;
                }

                const uint32_t stamp = (tid << 24) ^ (i * 2654435761u);
                constexpr auto words = static_cast<unsigned>(0x1000 / sizeof(uint32_t));
                for (unsigned w = 0; w < words; ++w)
                {
                    page[w] = stamp + w;
                }
                for (unsigned w = 0; w < words; ++w)
                {
                    if (page[w] != stamp + w)
                    {
                        ok.store(false);
                        break;
                    }
                }

                VirtualFree(page, 0, MEM_RELEASE);
            }
        });

        if (!ok.load())
        {
            puts("  memory churn: allocation failed or page contents corrupted");
            return false;
        }
        return true;
    }
}

#define RUN(func, name)                        \
    {                                          \
        printf("Running stress '" name "': "); \
        fflush(stdout);                        \
        const bool res = func();               \
        valid &= res;                          \
        puts(res ? "Success" : "Fail");        \
    }

int main(const int argc, const char* argv[])
{
    if (argc >= 2)
    {
        const auto parsed = std::strtol(argv[1], nullptr, 10);
        if (parsed > 0)
        {
            g_scale = static_cast<unsigned>(parsed);
        }
    }

    printf("stress-sample: %u threads, scale %u\n", kThreads, g_scale);

    bool valid = true;
    RUN(test_interlocked, "Interlocked")
    RUN(test_critical_section, "Critical Section")
    RUN(test_semaphore, "Semaphore")
    RUN(test_event_pingpong, "Event Ping-Pong")
    RUN(test_memory_churn, "Memory Churn")

    puts(valid ? "ALL STRESS TESTS PASSED" : "STRESS TESTS FAILED");
    return valid ? 0 : 1;
}
