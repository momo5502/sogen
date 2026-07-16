#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main(int argc, char** argv)
{
    uint64_t iterations = 2000000000ULL;
    if (argc > 1)
    {
        iterations = strtoull(argv[1], nullptr, 10);
    }

    LARGE_INTEGER freq{};
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    uint64_t acc = 0;
    for (uint64_t i = 0; i < iterations; ++i)
    {
        acc += i;
        acc ^= (acc << 13);
        acc ^= (acc >> 7);
        acc ^= (acc << 17);
    }

    QueryPerformanceCounter(&end);

    const double elapsed_seconds = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);

    printf("iterations=%llu result=%llu elapsed_seconds=%.6f\n", static_cast<unsigned long long>(iterations),
           static_cast<unsigned long long>(acc), elapsed_seconds);

    return 0;
}
