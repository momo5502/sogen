#define ICICLE_EMULATOR_IMPL
#include "icicle_x64_emulator.hpp"

extern "C" void test_rust();

namespace icicle
{
    std::unique_ptr<x64_emulator> create_x64_emulator()
    {
        test_rust();

        return {};
    }
}
