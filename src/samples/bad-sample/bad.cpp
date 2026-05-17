#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <platform/compiler.hpp>

#define THE_SIZE 30

extern "C" NO_INLINE EXPORT_SYMBOL void vulnerable(const uint8_t* data, const size_t size)
{
    if (size < 10)
    {
        return;
    }

    if (data[9] != 'A')
    {
        return;
    }

    if (data[8] != 'B')
    {
        return;
    }

    if (data[7] != 'C')
    {
        return;
    }

    if (data[2] != 'V')
    {
        return;
    }

    if (data[4] != 'H')
    {
        return;
    }

    if (size < 100)
    {
        return;
    }

    *reinterpret_cast<int*>(1) = 1;
}

std::array<uint8_t, THE_SIZE> buffer{};

int main(int argc, const char* argv[])
{
    const void* input = buffer.data();
    auto size = buffer.size();

    if (argc > 1)
    {
        input = argv[1];
        size = strlen(argv[1]);
    }

    vulnerable(reinterpret_cast<uint8_t*>(const_cast<void*>(input)), size);
    return 0;
}
