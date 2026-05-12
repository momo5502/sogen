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

namespace
{
    std::unique_ptr<x86_64_emulator> create_backend(backend_type backend)
    {
        switch (backend)
        {
        case backend_type::unicorn:
            return unicorn::create_x86_64_emulator();

#if MOMO_ENABLE_RUST_CODE
        case backend_type::icicle:
            // TODO: Add proper handling for WOW64 case (x64 -> x86 emulation is not supported yet).
            // icicle does not support automatic cross-architecture conversion from x64 to x86.
            // therefore WOW64 programs are naturally not supported to run.
            return icicle::create_x86_64_emulator();
#endif

#if defined(_WIN64) && !defined(__MINGW64__)
        case backend_type::whp:
            return whp::create_x86_64_emulator();
#endif
        }

        throw std::runtime_error("Requested backend is not available on this platform");
    }
}

std::unique_ptr<x86_64_emulator> create_x86_64_emulator(backend_type backend)
{
    return create_backend(backend);
}
