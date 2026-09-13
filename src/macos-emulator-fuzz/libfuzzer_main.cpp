// libFuzzer entry point. Build with clang: -fsanitize=fuzzer,address (see CMakeLists).

#include "fuzz_target.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    sogen::fuzz::run({data, size});
    return 0;
}
