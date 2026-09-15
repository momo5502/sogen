#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace sogen
{
    // Stand-in for SystemFunction040/041 (RtlEncryptMemory / RtlDecryptMemory). Same 16-byte
    // repeating XOR as KsecDD IOCTL 0x39000E..0x390022. The transform is involutive.
    inline constexpr std::array<uint8_t, 16> k_ksec_memory_crypt_key{
        'S', 'O', 'G', 'E', 'N', 'K', 'S', 'E', 'C', 'D', 'D', 'K', 'E', 'Y', '1', '6',
    };

    inline void xor_ksec_memory(const std::span<uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return;
        }

        uint8_t* const data = bytes.data();
        const auto key_size = k_ksec_memory_crypt_key.size();
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            data[i] = static_cast<uint8_t>(data[i] ^ k_ksec_memory_crypt_key[i % key_size]);
        }
    }
} // namespace sogen
