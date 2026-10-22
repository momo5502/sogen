#include "backend_selection.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string_view>
#if !defined(_M_ARM64)
#include <unicorn_x86_64_emulator.hpp>
#include <unicorn_arm64_emulator.hpp>
#endif

#if SOGEN_ENABLE_RUST_CODE
#include <icicle_x86_64_emulator.hpp>
#endif

#if defined(_M_ARM64) && !defined(__MINGW64__)
#include <whp_arm64_emulator.hpp>
#elif defined(_WIN64) && !defined(__MINGW64__)
#include <whp_x86_64_emulator.hpp>
#endif

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__amd64__))
#include <kvm_x86_64_emulator.hpp>
#endif

#if defined(SOGEN_ENABLE_FEX)
#include <fex_x86_64_emulator.hpp>
#endif

#if defined(SOGEN_ENABLE_HVF)
#include <hvf_arm64_emulator.hpp>
#endif

using namespace std::literals;

namespace sogen
{
    namespace
    {
#if !defined(_M_ARM64) || SOGEN_ENABLE_RUST_CODE
        std::unique_ptr<x86_64_emulator> create_backend(backend_type backend, [[maybe_unused]] const size_t vcpu_count)
        {
            switch (backend)
            {
#if !defined(_M_ARM64)
            case backend_type::unicorn:
                return unicorn::create_x86_64_emulator();
#endif

#if SOGEN_ENABLE_RUST_CODE
            case backend_type::icicle:
                // TODO: Add proper handling for WOW64 case (x64 -> x86 emulation is not supported yet).
                // icicle does not support automatic cross-architecture conversion from x64 to x86.
                // therefore WOW64 programs are naturally not supported to run.
                return icicle::create_x86_64_emulator();
#endif

            case backend_type::whp:
#if defined(_WIN64) && !defined(_M_ARM64) && !defined(__MINGW64__)
                return whp::create_x86_64_emulator(vcpu_count);
#else
                break;
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
#endif
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(backend_type backend, const size_t vcpu_count)
    {
#if defined(_M_ARM64) && !SOGEN_ENABLE_RUST_CODE
        (void)backend;
        (void)vcpu_count;
        throw std::runtime_error("No x86-64 guest backend is available in this Windows ARM64 build");
#else
        auto emulator = create_backend(backend, vcpu_count);
        if (vcpu_count > 1 && !emulator->supports_multiple_vcpus())
        {
            throw std::invalid_argument("The " + emulator->get_name() + " backend does not support multiple vCPUs");
        }

        return emulator;
#endif
    }

    std::unique_ptr<arm64_emulator> create_arm64_emulator(const backend_type backend, const size_t vcpu_count)
    {
        std::unique_ptr<arm64_emulator> emulator{};
        switch (backend)
        {
#if !defined(_M_ARM64)
        case backend_type::unicorn:
            emulator = unicorn::create_arm64_emulator();
            break;
#endif

#if defined(_M_ARM64) && !defined(__MINGW64__)
        case backend_type::whp:
            return whp::create_arm64_emulator(vcpu_count);
#endif

#if defined(SOGEN_ENABLE_HVF)
        case backend_type::hvf:
            return hvf::create_arm64_emulator(vcpu_count);
#endif

        default:
            break;
        }

        if (!emulator)
        {
            throw std::runtime_error("Requested ARM64 guest backend is not available on this platform");
        }

        if (vcpu_count > 1 && !emulator->supports_multiple_vcpus())
        {
            throw std::invalid_argument("The " + emulator->get_name() + " backend does not support multiple vCPUs");
        }

        return emulator;
    }

    backend_type get_arm64_emulator_backend_from_environment()
    {
#if defined(_M_ARM64) && !defined(__MINGW64__)
        return backend_type::whp;
#else
        const auto enabled = [](const char* name) {
            const auto* env = getenv(name);
            return env && (env == "1"sv || env == "true"sv);
        };
        if (enabled("EMULATOR_UNICORN"))
        {
            return backend_type::unicorn;
        }
#if defined(SOGEN_ENABLE_HVF)
        if (hvf::is_available())
        {
            return backend_type::hvf;
        }
#endif
        return backend_type::unicorn;
#endif
    }

    std::unique_ptr<arm64_emulator> create_arm64_emulator_from_environment(const size_t vcpu_count)
    {
        return create_arm64_emulator(get_arm64_emulator_backend_from_environment(), vcpu_count);
    }

    std::unique_ptr<guest_machine> create_emulator_for_machine(const machine_type machine, const std::optional<backend_type> backend,
                                                               const size_t vcpu_count)
    {
        switch (machine)
        {
        case machine_type::x86_64:
            return backend ? create_x86_64_emulator(*backend, vcpu_count) : create_x86_64_emulator_from_environment(vcpu_count);
        case machine_type::arm64:
            return backend ? create_arm64_emulator(*backend, vcpu_count) : create_arm64_emulator_from_environment(vcpu_count);
        }

        throw std::invalid_argument("Unsupported machine type");
    }

    backend_type get_x86_64_emulator_backend_from_environment()
    {
#if defined(_M_ARM64) && !SOGEN_ENABLE_RUST_CODE
        throw std::runtime_error("No x86-64 guest backend is available in this Windows ARM64 build");
#else
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

        return backend;
#endif
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator_from_environment(const size_t vcpu_count)
    {
#if defined(_M_ARM64) && !SOGEN_ENABLE_RUST_CODE
        (void)vcpu_count;
        throw std::runtime_error("No x86-64 guest backend is available in this Windows ARM64 build");
#else
        return create_x86_64_emulator(get_x86_64_emulator_backend_from_environment(), vcpu_count);
#endif
    }
} // namespace sogen
