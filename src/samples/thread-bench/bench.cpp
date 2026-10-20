#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
    using clock_type = std::chrono::steady_clock;

    double scale = 1;
    volatile uint64_t sink{};

    uint64_t mix(uint64_t x)
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    }

    void run_threads(const unsigned count, const std::function<void(unsigned)>& body)
    {
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < count; ++i)
        {
            threads.emplace_back([&, i] {
                ++ready;
                while (!go.load())
                {
                    std::this_thread::yield();
                }
                body(i);
            });
        }

        while (ready.load() != count)
        {
            std::this_thread::yield();
        }

        go = true;
        for (auto& thread : threads)
        {
            thread.join();
        }
    }

    void cpu(const unsigned threads)
    {
        run_threads(threads, [](const unsigned index) {
            uint64_t x = index + 1;
            for (uint64_t i = 0; i < 150'000'000 * scale; ++i)
            {
                x = mix(x);
            }
            sink = x;
        });
    }

    void fpu(const unsigned threads)
    {
        run_threads(threads, [](const unsigned index) {
            double x = index + 1;
            for (uint64_t i = 0; i < 6'000'000 * scale; ++i)
            {
                x = std::sqrt(x) * 1.0000001 + std::sin(x) * 0.5 + 1.0;
            }
            sink = static_cast<uint64_t>(x);
        });
    }

    void memory_random(const unsigned threads)
    {
        run_threads(threads, [](const unsigned index) {
            constexpr size_t count = (32u << 20) / sizeof(uint32_t);
            std::vector<uint32_t> chain(count);
            for (size_t i = 0; i < count; ++i)
            {
                chain[i] = static_cast<uint32_t>(i);
            }

            uint64_t seed = index + 7;
            for (size_t i = count - 1; i > 0; --i)
            {
                seed = mix(seed);
                std::swap(chain[i], chain[seed % (i + 1)]);
            }

            uint32_t at = 0;
            for (uint64_t i = 0; i < 6'000'000 * scale; ++i)
            {
                at = chain[at];
            }
            sink = at;
        });
    }

    void memory_stream(const unsigned threads)
    {
        run_threads(threads, [](const unsigned index) {
            std::vector<uint64_t> data(8u << 20, index);
            uint64_t sum = 0;
            for (uint64_t pass = 0; pass < 12 * scale; ++pass)
            {
                for (auto& value : data)
                {
                    value += pass;
                    sum += value;
                }
            }
            sink = sum;
        });
    }

    void atomic_shared(const unsigned threads)
    {
        alignas(64) static volatile LONG64 counter;
        counter = 0;
        run_threads(threads, [](const unsigned) {
            for (uint64_t i = 0; i < 4'000'000 * scale; ++i)
            {
                InterlockedIncrement64(&counter);
            }
        });
    }

    void atomic_private(const unsigned threads)
    {
        run_threads(threads, [](const unsigned) {
            alignas(64) volatile LONG64 counter = 0;
            for (uint64_t i = 0; i < 20'000'000 * scale; ++i)
            {
                InterlockedIncrement64(&counter);
            }
        });
    }

    void critical_section(const unsigned threads)
    {
        static CRITICAL_SECTION section;
        static uint64_t counter;
        InitializeCriticalSectionAndSpinCount(&section, 4000);
        counter = 0;
        run_threads(threads, [](const unsigned) {
            for (uint64_t i = 0; i < 1'000'000 * scale; ++i)
            {
                EnterCriticalSection(&section);
                ++counter;
                LeaveCriticalSection(&section);
            }
        });
        DeleteCriticalSection(&section);
    }

    void srw_lock(const unsigned threads)
    {
        static SRWLOCK lock = SRWLOCK_INIT;
        static uint64_t counter;
        counter = 0;
        run_threads(threads, [](const unsigned) {
            for (uint64_t i = 0; i < 1'000'000 * scale; ++i)
            {
                AcquireSRWLockExclusive(&lock);
                ++counter;
                ReleaseSRWLockExclusive(&lock);
            }
        });
    }

    void heap(const unsigned threads)
    {
        run_threads(threads, [](const unsigned index) {
            std::vector<void*> live(256, nullptr);
            uint64_t seed = index + 3;
            for (uint64_t i = 0; i < 1'500'000 * scale; ++i)
            {
                seed = mix(seed);
                auto& slot = live[seed % live.size()];
                if (slot)
                {
                    free(slot);
                }
                slot = malloc(16 + (seed >> 20) % 2048);
                static_cast<char*>(slot)[0] = 1;
            }
            for (auto* slot : live)
            {
                free(slot);
            }
        });
    }

    void virtual_alloc(const unsigned threads)
    {
        run_threads(threads, [](const unsigned) {
            for (uint64_t i = 0; i < 6'000 * scale; ++i)
            {
                auto* memory = static_cast<char*>(VirtualAlloc(nullptr, 256 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                for (size_t offset = 0; offset < 256 * 1024; offset += 4096)
                {
                    memory[offset] = 1;
                }
                VirtualFree(memory, 0, MEM_RELEASE);
            }
        });
    }

    std::atomic<uint64_t> integrity_errors{0};

    void memory_integrity(const unsigned threads)
    {
        run_threads(threads, [](const unsigned index) {
            constexpr size_t sizes[] = {4096, 16384, 65536, 262144, 1u << 20};
            uint64_t seed = index + 11;
            for (uint64_t i = 0; i < 1'500 * scale; ++i)
            {
                seed = mix(seed);
                const auto size = sizes[seed % std::size(sizes)];
                auto* memory = static_cast<uint64_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (!memory)
                {
                    ++integrity_errors;
                    continue;
                }

                const auto words = size / sizeof(uint64_t);
                for (size_t word = 0; word < words; word += 8)
                {
                    if (memory[word] != 0)
                    {
                        ++integrity_errors;
                        break;
                    }
                }

                const auto pattern = (static_cast<uint64_t>(index) << 56) ^ (i * 0x9E3779B97F4A7C15ull);
                for (size_t word = 0; word < words; word += 8)
                {
                    memory[word] = pattern + word;
                }

                if (i % 4 == 0)
                {
                    SwitchToThread();
                }

                for (size_t word = 0; word < words; word += 8)
                {
                    if (memory[word] != pattern + word)
                    {
                        ++integrity_errors;
                        break;
                    }
                }

                VirtualFree(memory, 0, MEM_RELEASE);
            }
        });
    }

    void syscall_event(const unsigned threads)
    {
        run_threads(threads, [](const unsigned) {
            const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            for (uint64_t i = 0; i < 300'000 * scale; ++i)
            {
                SetEvent(event);
                WaitForSingleObject(event, INFINITE);
            }
            CloseHandle(event);
        });
    }

    void syscall_yield(const unsigned threads)
    {
        run_threads(threads, [](const unsigned) {
            for (uint64_t i = 0; i < 300'000 * scale; ++i)
            {
                SwitchToThread();
            }
        });
    }

    void file_attributes(const unsigned threads)
    {
        run_threads(threads, [](const unsigned) {
            uint64_t found = 0;
            for (uint64_t i = 0; i < 2'000 * scale; ++i)
            {
                found += GetFileAttributesW(L"C:\\Windows\\System32\\ntdll.dll") != INVALID_FILE_ATTRIBUTES;
            }
            sink = found;
        });
    }

    void event_pingpong(const unsigned threads)
    {
        const auto pairs = std::max(1u, threads / 2);
        std::vector<HANDLE> events;
        for (unsigned i = 0; i < pairs * 2; ++i)
        {
            events.push_back(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        }

        run_threads(pairs * 2, [&](const unsigned index) {
            const auto pair = index / 2;
            const auto mine = events[pair * 2 + (index & 1)];
            const auto theirs = events[pair * 2 + ((index & 1) ^ 1)];
            for (uint64_t i = 0; i < 5'000 * scale; ++i)
            {
                if (index & 1)
                {
                    WaitForSingleObject(mine, INFINITE);
                    SetEvent(theirs);
                }
                else
                {
                    SetEvent(theirs);
                    WaitForSingleObject(mine, INFINITE);
                }
            }
        });

        for (const auto event : events)
        {
            CloseHandle(event);
        }
    }

    void thread_create(const unsigned threads)
    {
        run_threads(threads, [](const unsigned) {
            for (uint64_t i = 0; i < 300 * scale; ++i)
            {
                std::thread([] { sink = sink + 1; }).join();
            }
        });
    }

    struct workload
    {
        const char* name;
        void (*run)(unsigned);
    };

    constexpr workload workloads[] = {
        {"cpu", cpu},
        {"fpu", fpu},
        {"memory_random", memory_random},
        {"memory_stream", memory_stream},
        {"atomic_private", atomic_private},
        {"atomic_shared", atomic_shared},
        {"critical_section", critical_section},
        {"srw_lock", srw_lock},
        {"heap", heap},
        {"virtual_alloc", virtual_alloc},
        {"memory_integrity", memory_integrity},
        {"syscall_event", syscall_event},
        {"syscall_yield", syscall_yield},
        {"file_attributes", file_attributes},
        {"event_pingpong", event_pingpong},
        {"thread_create", thread_create},
    };
}

int main(int argc, char** argv)
{
    std::vector<unsigned> thread_counts;
    std::vector<std::string> selected;
    unsigned repeats = 1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "-s" && i + 1 < argc)
        {
            scale = std::max(0.001, std::strtod(argv[++i], nullptr));
        }
        else if (argument == "-r" && i + 1 < argc)
        {
            repeats = std::max(1u, static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10)));
        }
        else if (argument == "-w" && i + 1 < argc)
        {
            selected.emplace_back(argv[++i]);
        }
        else
        {
            thread_counts.push_back(std::max(1u, static_cast<unsigned>(std::strtoul(argument.c_str(), nullptr, 10))));
        }
    }

    if (thread_counts.empty())
    {
        thread_counts = {1, 2, 4};
    }

    printf("workload,threads,ms\n");
    for (const auto& entry : workloads)
    {
        if (!selected.empty() && std::find(selected.begin(), selected.end(), entry.name) == selected.end())
        {
            continue;
        }

        for (const auto threads : thread_counts)
        {
            double best = 0;
            for (unsigned repeat = 0; repeat < repeats; ++repeat)
            {
                const auto start = clock_type::now();
                entry.run(threads);
                const auto elapsed = std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
                best = repeat == 0 ? elapsed : std::min(best, elapsed);
            }

            printf("%s,%u,%.1f\n", entry.name, threads, best);
            fflush(stdout);
        }
    }

    if (integrity_errors.load() != 0)
    {
        printf("INTEGRITY FAILURE: %llu errors\n", static_cast<unsigned long long>(integrity_errors.load()));
        return 2;
    }

    return 0;
}
