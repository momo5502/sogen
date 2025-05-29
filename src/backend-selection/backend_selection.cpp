#include "backend_selection.hpp"

#include <string_view>
#include <unicorn_x86_64_emulator.hpp>

#if MOMO_ENABLE_RUST_CODE
#include <icicle_x86_64_emulator.hpp>
#endif

using namespace std::literals;

std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
{
#if MOMO_ENABLE_RUST_CODE
    const auto* env = getenv("EMULATOR_ICICLE");
    if (env && (env == "1"sv || env == "true"sv))
    {
        return icicle::create_x86_64_emulator();
    }
#endif

    return unicorn::create_x86_64_emulator();
}
