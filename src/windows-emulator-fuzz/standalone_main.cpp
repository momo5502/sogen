// Standalone replay driver — no fuzzing engine required, builds on any toolchain.
// Feeds each file given on the command line through the fuzz target once. Use for smoke tests,
// corpus regression runs, and reproducing crashes from a saved input.

#include "fuzz_target.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        std::ifstream file(argv[i], std::ios::binary);
        const std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        sogen::fuzz::run(buffer);
    }

    return 0;
}
