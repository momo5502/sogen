#pragma once

#include <array>
#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

namespace utils::cpu_features
{
    namespace detail
    {
        using CpuidRegs = std::array<std::uint32_t, 4>;

        [[nodiscard]] inline CpuidRegs cpuid(std::uint32_t leaf, std::uint32_t subleaf = 0) noexcept
        {
            CpuidRegs regs{};

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
            int out[4]{};
            __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));

            regs[0] = static_cast<std::uint32_t>(out[0]); // EAX
            regs[1] = static_cast<std::uint32_t>(out[1]); // EBX
            regs[2] = static_cast<std::uint32_t>(out[2]); // ECX
            regs[3] = static_cast<std::uint32_t>(out[3]); // EDX
#elif defined(__i386__) || defined(__x86_64__)
            std::uint32_t eax{};
            std::uint32_t ebx{};
            std::uint32_t ecx{};
            std::uint32_t edx{};

            __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);

            regs = {eax, ebx, ecx, edx};
#endif

            return regs;
        }

        [[nodiscard]] inline std::uint64_t xgetbv(std::uint32_t index) noexcept
        {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
            return _xgetbv(index);
#elif defined(__i386__) || defined(__x86_64__)
            std::uint32_t eax{};
            std::uint32_t edx{};

            __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));

            return (static_cast<std::uint64_t>(edx) << 32) | eax;
#else
            return 0;
#endif
        }
    }

    [[nodiscard]] inline bool avx_enabled() noexcept
    {
#if defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)
        const auto regs = detail::cpuid(1);
        const auto ecx = regs[2];

        constexpr std::uint32_t xsave_bit = 1u << 26;
        constexpr std::uint32_t osxsave_bit = 1u << 27;
        constexpr std::uint32_t avx_bit = 1u << 28;

        constexpr std::uint32_t required_cpu_bits = xsave_bit | osxsave_bit | avx_bit;

        if ((ecx & required_cpu_bits) != required_cpu_bits)
        {
            return false;
        }

        const auto xcr0 = detail::xgetbv(0);

        constexpr std::uint64_t xmm_state = 1ull << 1;
        constexpr std::uint64_t ymm_state = 1ull << 2;

        return (xcr0 & (xmm_state | ymm_state)) == (xmm_state | ymm_state);
#else
        return false;
#endif
    }
}
