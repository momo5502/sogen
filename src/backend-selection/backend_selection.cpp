#include "backend_selection.hpp"

#include <string_view>
#include <unicorn_x86_64_emulator.hpp>

#if SOGEN_ENABLE_RUST_CODE
#include <icicle_x86_64_emulator.hpp>
#endif

#if defined(_WIN64) && !defined(__MINGW64__)
#include <whp_x86_64_emulator.hpp>
#endif

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__amd64__))
#include <kvm_x86_64_emulator.hpp>
#endif

#if defined(SOGEN_ENABLE_FEX)
#include <fex_x86_64_emulator.hpp>
#endif

using namespace std::literals;

namespace sogen
{
    namespace
    {
        std::unique_ptr<x86_64_emulator> create_backend(backend_type backend, [[maybe_unused]] const size_t vcpu_count)
        {
            switch (backend)
            {
            case backend_type::unicorn:
                return unicorn::create_x86_64_emulator();

#if SOGEN_ENABLE_RUST_CODE
            case backend_type::icicle:
                // TODO: Add proper handling for WOW64 case (x64 -> x86 emulation is not supported yet).
                // icicle does not support automatic cross-architecture conversion from x64 to x86.
                // therefore WOW64 programs are naturally not supported to run.
                return icicle::create_x86_64_emulator();
#endif

#if defined(_WIN64) && !defined(__MINGW64__)
            case backend_type::whp:
                return whp::create_x86_64_emulator(vcpu_count);
#endif

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__amd64__))
            case backend_type::kvm:
                return kvm::create_x86_64_emulator();
#endif

#if defined(SOGEN_ENABLE_FEX)
            case backend_type::fex:
                return fex::create_x86_64_emulator();
#endif

            default:
                break;
            }

            throw std::runtime_error("Requested backend is not available on this platform");
        }
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(backend_type backend, const size_t vcpu_count)
    {
        auto emulator = create_backend(backend, vcpu_count);
        if (vcpu_count > 1 && !emulator->supports_multiple_vcpus())
        {
            throw std::invalid_argument("The " + emulator->get_name() + " backend does not support multiple vCPUs");
        }

        return emulator;
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator_from_environment(const size_t vcpu_count)
    {
        auto backend = backend_type::unicorn;

        {
            const auto* env = getenv("EMULATOR_WHP");
            if (env && (env == "1"sv || env == "true"sv))
            {
                backend = backend_type::whp;
            }
        }

        {
            const auto* env = getenv("EMULATOR_ICICLE");
            if (env && (env == "1"sv || env == "true"sv))
            {
                // TODO: Add proper handling for WOW64 case (x64 -> x86 emulation is not supported yet).
                // icicle does not support automatic cross-architecture conversion from x64 to x86.
                // therefore WOW64 programs are naturally not supported to run.
                backend = backend_type::icicle;
            }
        }

        {
            const auto* env = getenv("EMULATOR_KVM");
            if (env && (env == "1"sv || env == "true"sv))
            {
                backend = backend_type::kvm;
            }
        }

        {
            const auto* env = getenv("EMULATOR_FEX");
            if (env && (env == "1"sv || env == "true"sv))
            {
                backend = backend_type::fex;
            }
        }

        return create_x86_64_emulator(backend, vcpu_count);
    }
} // namespace sogen
