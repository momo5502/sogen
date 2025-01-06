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

    *(int*)1 = 1;
}

uint8_t buffer[THE_SIZE] = {};

int main(int argc, const char* argv[])
{
    const void* input = buffer;
    auto size = sizeof(buffer);

    if (argc > 1)
    {
        input = argv[1];
        size = strlen(argv[1]);
    }

    vulnerable((uint8_t*)input, size);
    return 0;
}
