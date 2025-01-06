#pragma once

#include <cstdint>

// used to retain original type "x"
#define EMULATOR_CAST(T, x) T

struct Emu32
{
};

struct Emu64
{
};

template <typename EmuArch>
struct EmulatorTraits;

template <>
struct EmulatorTraits<Emu32>
{
    using PVOID = std::uint32_t;
    using ULONG_PTR = std::uint32_t;
    using SIZE_T = std::uint32_t;
    using UNICODE = char16_t;
    using HANDLE = std::uint32_t;
};

template <>
struct EmulatorTraits<Emu64>
{
    using PVOID = std::uint64_t;
    using ULONG_PTR = std::uint64_t;
    using SIZE_T = std::uint64_t;
    using UNICODE = char16_t;
    using HANDLE = std::uint64_t;
};
