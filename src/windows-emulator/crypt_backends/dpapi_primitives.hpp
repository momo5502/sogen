#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace sogen::dpapi_primitives
{
    inline uint32_t load_be32(const uint8_t* p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }

    inline uint64_t load_be64(const uint8_t* p)
    {
        return (static_cast<uint64_t>(load_be32(p)) << 32) | load_be32(p + 4);
    }

    inline void store_be32(uint8_t* p, const uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }

    inline void store_be64(uint8_t* p, const uint64_t v)
    {
        store_be32(p, static_cast<uint32_t>(v >> 32));
        store_be32(p + 4, static_cast<uint32_t>(v));
    }

    inline uint32_t rotl32(const uint32_t x, const uint32_t n)
    {
        return (x << n) | (x >> (32 - n));
    }

    inline uint32_t rotr32(const uint32_t x, const uint32_t n)
    {
        return (x >> n) | (x << (32 - n));
    }

    inline uint64_t rotr64(const uint64_t x, const uint32_t n)
    {
        return (x >> n) | (x << (64 - n));
    }

    inline std::array<uint8_t, 20> sha1(const std::span<const uint8_t> data)
    {
        std::array<uint32_t, 5> h{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        auto* hv = h.data();

        const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
        std::array<uint8_t, 128> block{};
        size_t remaining = data.size();
        const uint8_t* ptr = data.data();

        auto process = [&](const uint8_t* chunk) {
            std::array<uint32_t, 80> w_arr{};
            auto* w = w_arr.data();
            for (int i = 0; i < 16; ++i)
            {
                w[i] = load_be32(chunk + static_cast<size_t>(i) * 4);
            }
            for (int i = 16; i < 80; ++i)
            {
                w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }

            uint32_t a = hv[0];
            uint32_t b = hv[1];
            uint32_t c = hv[2];
            uint32_t d = hv[3];
            uint32_t e = hv[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f{};
                uint32_t k{};
                if (i < 20)
                {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                }
                else if (i < 40)
                {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                }
                else if (i < 60)
                {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                }
                else
                {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }

                const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rotl32(b, 30);
                b = a;
                a = temp;
            }

            hv[0] += a;
            hv[1] += b;
            hv[2] += c;
            hv[3] += d;
            hv[4] += e;
        };

        while (remaining >= 64)
        {
            process(ptr);
            ptr += 64;
            remaining -= 64;
        }

        std::memcpy(block.data(), ptr, remaining);
        auto* padded = block.data();
        padded[remaining] = 0x80;
        if (remaining + 1 + 8 > 64)
        {
            process(block.data());
            std::memset(block.data(), 0, 64);
        }
        store_be64(block.data() + 56, bit_len);
        process(block.data());

        std::array<uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i)
        {
            store_be32(out.data() + static_cast<size_t>(i) * 4, hv[i]);
        }
        return out;
    }

    inline std::array<uint8_t, 64> sha512(const std::span<const uint8_t> data)
    {
        static constexpr std::array<uint64_t, 80> k{
            0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
            0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
            0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
            0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
            0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
            0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
            0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
            0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
            0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
            0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
            0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
            0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
            0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
            0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
        };
        const auto* kv = k.data();

        std::array<uint64_t, 8> h{0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                  0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
        auto* hv = h.data();

        const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
        std::array<uint8_t, 256> block{};
        size_t remaining = data.size();
        const uint8_t* ptr = data.data();

        auto process = [&](const uint8_t* chunk) {
            std::array<uint64_t, 80> w_arr{};
            auto* w = w_arr.data();
            for (int i = 0; i < 16; ++i)
            {
                w[i] = load_be64(chunk + static_cast<size_t>(i) * 8);
            }
            for (int i = 16; i < 80; ++i)
            {
                const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
                const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint64_t a = hv[0];
            uint64_t b = hv[1];
            uint64_t c = hv[2];
            uint64_t d = hv[3];
            uint64_t e = hv[4];
            uint64_t f = hv[5];
            uint64_t g = hv[6];
            uint64_t hh = hv[7];
            for (int i = 0; i < 80; ++i)
            {
                const uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
                const uint64_t ch = (e & f) ^ ((~e) & g);
                const uint64_t temp1 = hh + S1 + ch + kv[i] + w[i];
                const uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
                const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
                const uint64_t temp2 = S0 + maj;
                hh = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            hv[0] += a;
            hv[1] += b;
            hv[2] += c;
            hv[3] += d;
            hv[4] += e;
            hv[5] += f;
            hv[6] += g;
            hv[7] += hh;
        };

        while (remaining >= 128)
        {
            process(ptr);
            ptr += 128;
            remaining -= 128;
        }

        std::memcpy(block.data(), ptr, remaining);
        auto* padded = block.data();
        padded[remaining] = 0x80;
        if (remaining + 1 + 16 > 128)
        {
            process(block.data());
            std::memset(block.data(), 0, 128);
        }
        store_be64(block.data() + 120, bit_len);
        process(block.data());

        std::array<uint8_t, 64> out{};
        for (int i = 0; i < 8; ++i)
        {
            store_be64(out.data() + static_cast<size_t>(i) * 8, hv[i]);
        }
        return out;
    }

    inline std::array<uint8_t, 64> hmac_sha512(const std::span<const uint8_t> key, const std::span<const uint8_t> data)
    {
        constexpr size_t block_size = 128;
        std::array<uint8_t, block_size> key_block{};
        if (key.size() > block_size)
        {
            const auto hashed = sha512(key);
            std::memcpy(key_block.data(), hashed.data(), hashed.size());
        }
        else
        {
            std::memcpy(key_block.data(), key.data(), key.size());
        }

        std::array<uint8_t, block_size> ipad{};
        std::array<uint8_t, block_size> opad{};
        auto* ip = ipad.data();
        auto* op = opad.data();
        const auto* kb = key_block.data();
        for (size_t i = 0; i < block_size; ++i)
        {
            ip[i] = static_cast<uint8_t>(kb[i] ^ 0x36);
            op[i] = static_cast<uint8_t>(kb[i] ^ 0x5c);
        }

        std::vector<uint8_t> inner;
        inner.reserve(block_size + data.size());
        inner.insert(inner.end(), ipad.begin(), ipad.end());
        inner.insert(inner.end(), data.begin(), data.end());
        const auto inner_hash = sha512(inner);

        std::array<uint8_t, block_size + 64> outer{};
        std::memcpy(outer.data(), opad.data(), block_size);
        std::memcpy(outer.data() + block_size, inner_hash.data(), inner_hash.size());
        return sha512(std::span<const uint8_t>{outer.data(), outer.size()});
    }

    inline constexpr std::array<uint8_t, 256> k_sbox{
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59,
        0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1,
        0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83,
        0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
        0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
        0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
        0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee,
        0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
        0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6,
        0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
        0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1,
        0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
    };

    inline constexpr std::array<uint8_t, 256> k_inv_sbox{
        0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f,
        0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b,
        0x42, 0xfa, 0xc3, 0x4e, 0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8,
        0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda,
        0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84, 0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3,
        0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
        0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73, 0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9,
        0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
        0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4, 0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07,
        0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f,
        0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61, 0x17, 0x2b,
        0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
    };

    inline uint8_t xtime(const uint8_t x)
    {
        return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
    }

    inline uint8_t gmul(uint8_t a, uint8_t b)
    {
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i)
        {
            if ((b & 1) != 0)
            {
                p ^= a;
            }
            a = xtime(a);
            b >>= 1;
        }
        return p;
    }

    inline void sub_bytes(uint8_t* state, const uint8_t* sbox)
    {
        for (int i = 0; i < 16; ++i)
        {
            state[i] = sbox[state[i]];
        }
    }

    inline void shift_rows(uint8_t* state)
    {
        uint8_t t = state[1];
        state[1] = state[5];
        state[5] = state[9];
        state[9] = state[13];
        state[13] = t;
        t = state[2];
        state[2] = state[10];
        state[10] = t;
        t = state[6];
        state[6] = state[14];
        state[14] = t;
        t = state[15];
        state[15] = state[11];
        state[11] = state[7];
        state[7] = state[3];
        state[3] = t;
    }

    inline void inv_shift_rows(uint8_t* state)
    {
        uint8_t t = state[13];
        state[13] = state[9];
        state[9] = state[5];
        state[5] = state[1];
        state[1] = t;
        t = state[2];
        state[2] = state[10];
        state[10] = t;
        t = state[6];
        state[6] = state[14];
        state[14] = t;
        t = state[3];
        state[3] = state[7];
        state[7] = state[11];
        state[11] = state[15];
        state[15] = t;
    }

    inline void mix_columns(uint8_t* state)
    {
        for (int c = 0; c < 4; ++c)
        {
            const uint8_t a0 = state[c * 4];
            const uint8_t a1 = state[c * 4 + 1];
            const uint8_t a2 = state[c * 4 + 2];
            const uint8_t a3 = state[c * 4 + 3];
            state[c * 4] = static_cast<uint8_t>(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
            state[c * 4 + 1] = static_cast<uint8_t>(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
            state[c * 4 + 2] = static_cast<uint8_t>(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
            state[c * 4 + 3] = static_cast<uint8_t>(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
        }
    }

    inline void inv_mix_columns(uint8_t* state)
    {
        for (int c = 0; c < 4; ++c)
        {
            const uint8_t a0 = state[c * 4];
            const uint8_t a1 = state[c * 4 + 1];
            const uint8_t a2 = state[c * 4 + 2];
            const uint8_t a3 = state[c * 4 + 3];
            state[c * 4] = static_cast<uint8_t>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
            state[c * 4 + 1] = static_cast<uint8_t>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            state[c * 4 + 2] = static_cast<uint8_t>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
            state[c * 4 + 3] = static_cast<uint8_t>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }

    inline void add_round_key(uint8_t* state, const uint32_t* round_key)
    {
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t w = round_key[i];
            state[i * 4] ^= static_cast<uint8_t>(w >> 24);
            state[i * 4 + 1] ^= static_cast<uint8_t>(w >> 16);
            state[i * 4 + 2] ^= static_cast<uint8_t>(w >> 8);
            state[i * 4 + 3] ^= static_cast<uint8_t>(w);
        }
    }

    inline uint32_t sub_word(uint32_t w)
    {
        const auto* sbox = k_sbox.data();
        return (static_cast<uint32_t>(sbox[(w >> 24) & 0xff]) << 24) | (static_cast<uint32_t>(sbox[(w >> 16) & 0xff]) << 16) |
               (static_cast<uint32_t>(sbox[(w >> 8) & 0xff]) << 8) | static_cast<uint32_t>(sbox[w & 0xff]);
    }

    inline void expand_key_256(const uint8_t* key, uint32_t* round_keys)
    {
        for (int i = 0; i < 8; ++i)
        {
            round_keys[i] = load_be32(key + static_cast<size_t>(i) * 4);
        }

        static constexpr std::array<uint32_t, 8> rcon{0x01000000, 0x02000000, 0x04000000, 0x08000000,
                                                      0x10000000, 0x20000000, 0x40000000, 0x80000000};
        const auto* rc = rcon.data();
        for (int i = 8; i < 60; ++i)
        {
            uint32_t temp = round_keys[i - 1];
            if (i % 8 == 0)
            {
                temp = sub_word(rotl32(temp, 8)) ^ rc[i / 8 - 1];
            }
            else if (i % 8 == 4)
            {
                temp = sub_word(temp);
            }
            round_keys[i] = round_keys[i - 8] ^ temp;
        }
    }

    inline void aes256_encrypt_block(const uint32_t* round_keys, uint8_t* block)
    {
        add_round_key(block, round_keys);
        for (int round = 1; round < 14; ++round)
        {
            sub_bytes(block, k_sbox.data());
            shift_rows(block);
            mix_columns(block);
            add_round_key(block, round_keys + round * 4);
        }
        sub_bytes(block, k_sbox.data());
        shift_rows(block);
        add_round_key(block, round_keys + 56);
    }

    inline void aes256_decrypt_block(const uint32_t* round_keys, uint8_t* block)
    {
        add_round_key(block, round_keys + 56);
        for (int round = 13; round >= 1; --round)
        {
            inv_shift_rows(block);
            sub_bytes(block, k_inv_sbox.data());
            add_round_key(block, round_keys + round * 4);
            inv_mix_columns(block);
        }
        inv_shift_rows(block);
        sub_bytes(block, k_inv_sbox.data());
        add_round_key(block, round_keys);
    }

    inline bool aes256_cbc_encrypt(const std::span<const uint8_t, 32> key, const std::span<const uint8_t> plaintext,
                                   std::vector<uint8_t>& ciphertext)
    {
        std::array<uint32_t, 60> round_keys{};
        expand_key_256(key.data(), round_keys.data());

        const size_t pad = 16 - (plaintext.size() % 16);
        ciphertext.resize(plaintext.size() + pad);
        if (!plaintext.empty())
        {
            std::memcpy(ciphertext.data(), plaintext.data(), plaintext.size());
        }
        std::memset(ciphertext.data() + plaintext.size(), static_cast<int>(pad), pad);

        std::array<uint8_t, 16> prev{};
        for (size_t off = 0; off < ciphertext.size(); off += 16)
        {
            std::array<uint8_t, 16> block{};
            std::memcpy(block.data(), ciphertext.data() + off, 16);
            auto* b = block.data();
            const auto* p = prev.data();
            for (int i = 0; i < 16; ++i)
            {
                b[i] ^= p[i];
            }
            aes256_encrypt_block(round_keys.data(), b);
            std::memcpy(ciphertext.data() + off, b, 16);
            std::memcpy(prev.data(), b, 16);
        }
        return true;
    }

    inline bool aes256_cbc_decrypt(const std::span<const uint8_t, 32> key, const std::span<const uint8_t> ciphertext,
                                   std::vector<uint8_t>& plaintext)
    {
        if (ciphertext.empty() || (ciphertext.size() % 16) != 0)
        {
            return false;
        }

        std::array<uint32_t, 60> round_keys{};
        expand_key_256(key.data(), round_keys.data());

        plaintext.resize(ciphertext.size());
        auto* plain = plaintext.data();
        std::array<uint8_t, 16> prev{};
        for (size_t off = 0; off < ciphertext.size(); off += 16)
        {
            std::array<uint8_t, 16> block{};
            std::memcpy(block.data(), ciphertext.data() + off, 16);
            std::array<uint8_t, 16> decoded{};
            std::memcpy(decoded.data(), block.data(), 16);
            aes256_decrypt_block(round_keys.data(), decoded.data());
            const auto* d = decoded.data();
            const auto* p = prev.data();
            for (int i = 0; i < 16; ++i)
            {
                plain[off + static_cast<size_t>(i)] = static_cast<uint8_t>(d[i] ^ p[i]);
            }
            std::memcpy(prev.data(), block.data(), 16);
        }

        const uint8_t pad = plaintext.back();
        if (pad == 0 || pad > 16 || pad > plaintext.size())
        {
            return false;
        }
        for (size_t i = plaintext.size() - pad; i < plaintext.size(); ++i)
        {
            if (plain[i] != pad)
            {
                return false;
            }
        }
        plaintext.resize(plaintext.size() - pad);
        return true;
    }

    inline bool constant_time_equal(const std::span<const uint8_t> a, const std::span<const uint8_t> b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        uint8_t diff = 0;
        const auto* aa = a.data();
        const auto* bb = b.data();
        for (size_t i = 0; i < a.size(); ++i)
        {
            diff = static_cast<uint8_t>(diff | (aa[i] ^ bb[i]));
        }
        return diff == 0;
    }
}
