#include "backend_selection.hpp"

#include <string_view>
#include <unicorn_x86_64_emulator.hpp>

#if MOMO_ENABLE_RUST_CODE
#include <icicle_x86_64_emulator.hpp>
#endif

#if defined(_WIN64) && !defined(__MINGW64__)
#include <whp_x86_64_emulator.hpp>
#endif

using namespace std::literals;

std::unique_ptr<x86_64_emulator> create_x86_64_emulator()
{
#if defined(_WIN64) && !defined(__MINGW64__)
    {
        const auto* env = getenv("EMULATOR_WHP");
        if (env && (env == "1"sv || env == "true"sv))
        {
            return whp::create_x86_64_emulator();
        }
    }
#endif

#if MOMO_ENABLE_RUST_CODE
    {
        const auto* env = getenv("EMULATOR_ICICLE");
        if (env && (env == "1"sv || env == "true"sv))
        {
            // TODO: Add proper handling for WOW64 case (x64 -> x86 emulation is not supported yet).
            // icicle does not support automatic cross-architecture conversion from x64 to x86.
            // therefore WOW64 programs are naturally not supported to run.
            return icicle::create_x86_64_emulator();
        }
    }
#endif

    return unicorn::create_x86_64_emulator();
}
