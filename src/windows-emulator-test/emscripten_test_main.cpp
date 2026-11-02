#include <platform/compiler.hpp>

#ifdef OS_EMSCRIPTEN

#include <gtest/gtest.h>
#include <emscripten.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    const auto result = RUN_ALL_TESTS();
    // Async tests can unwind main before its real exit status is available.
    emscripten_force_exit(result);
}

#endif
