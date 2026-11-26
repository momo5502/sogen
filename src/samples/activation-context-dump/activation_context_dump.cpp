#include <windows.h>
#include <winternl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>

namespace
{
    constexpr uint32_t activation_context_magic = 0x78746341;
    constexpr size_t activation_context_header_size = 0x20;
    constexpr size_t total_size_offset = 12;

    // winternl.h does not declare PEB.SystemDefaultActivationContextData; this is its offset in the PEB of
    // a 64-bit and of a 32-bit process.
    constexpr size_t system_default_activation_context_offset = sizeof(void*) == 8 ? 0x308 : 0x200;

    const std::byte* find_system_default_activation_context(size_t& size)
    {
        const auto* const peb = reinterpret_cast<const std::byte*>(NtCurrentTeb()->ProcessEnvironmentBlock);
        const auto* const data = *reinterpret_cast<const std::byte* const*>(peb + system_default_activation_context_offset);
        if (!data)
        {
            return nullptr;
        }

        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(data, &region, sizeof(region)) || region.State != MEM_COMMIT)
        {
            return nullptr;
        }

        const auto readable = static_cast<size_t>(static_cast<const std::byte*>(region.BaseAddress) + region.RegionSize - data);
        if (readable < activation_context_header_size)
        {
            return nullptr;
        }

        uint32_t magic{};
        uint32_t total_size{};
        std::memcpy(&magic, data, sizeof(magic));
        std::memcpy(&total_size, data + total_size_offset, sizeof(total_size));
        if (magic != activation_context_magic || total_size < activation_context_header_size || total_size > readable)
        {
            return nullptr;
        }

        size = total_size;
        return data;
    }
}

int main(const int argc, const char* const* const argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: activation-context-dump <output file>\n";
        return 2;
    }

    size_t size{};
    const auto* const data = find_system_default_activation_context(size);
    if (!data)
    {
        std::cerr << "This process has no system default activation context\n";
        return 1;
    }

    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    output.close();
    if (!output)
    {
        std::cerr << "Failed to write " << argv[1] << '\n';
        return 1;
    }

    return 0;
}
