#pragma once

// Host-side runtime the generated Steam thunks build on. Includes the real SDK headers so the thunks
// call genuine interface methods (compiler-correct ABI). Serialization mirrors the guest invoker.
//
// SECURITY: everything here treats the guest-supplied blob as hostile -- reads are bounded to the input,
// variable payloads are length-capped, and every guest-influenced allocation is clamped. Nothing here may
// read or write outside the buffers it is given.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#include "steam_api.h"
#include "steam_gameserver.h"

namespace sogen::steam_host
{
    enum : int
    {
        steam_host_ok = 0,
        steam_host_unsupported = -1,
        steam_host_unknown_method = -2,
        steam_host_unknown_interface = -3,
    };

    inline constexpr size_t max_payload = 16u << 20; // 16 MiB cap on any single variable payload

    // Sequential reader over the guest input blob. Never reads past [begin,end).
    struct steam_host_reader
    {
        const unsigned char* p;
        const unsigned char* end;

        void get(void* dst, size_t n)
        {
            if (p + n <= end && p + n >= p)
            {
                std::memcpy(dst, p, n);
                p += n;
            }
            else
            {
                std::memset(dst, 0, n); // truncated/hostile input: zero-fill rather than over-read
                p = end;
            }
        }
        // Reads a by-value scalar from its fixed 8-byte wire slot into the host's native width (see the
        // guest's put_scalar). Arch-agnostic: only the low `n` bytes are taken.
        void get_scalar(void* dst, size_t n)
        {
            unsigned char slot[8];
            get(slot, sizeof(slot));
            if (n > sizeof(slot))
            {
                n = sizeof(slot);
            }
            std::memcpy(dst, slot, n);
        }
        const char* get_cstr()
        {
            const char* s = reinterpret_cast<const char*>(p);
            while (p < end && *p)
            {
                ++p;
            }
            if (p < end)
            {
                ++p; // consume the NUL
            }
            else
            {
                return ""; // unterminated: hand back a safe empty string
            }
            return s;
        }
        std::vector<unsigned char> get_var()
        {
            uint32_t len = 0;
            get(&len, sizeof(len));
            const size_t avail = (p <= end) ? static_cast<size_t>(end - p) : 0;
            if (len > avail)
            {
                len = static_cast<uint32_t>(avail);
            }
            if (len > max_payload)
            {
                len = static_cast<uint32_t>(max_payload);
            }
            std::vector<unsigned char> v(p, p + len);
            p += len;
            return v;
        }
    };

    // Appends the reply: out-parameter payloads (in order) then the scalar/struct/string return.
    struct steam_host_writer
    {
        std::vector<unsigned char>& out;
        uint64_t& ret;

        void put_out(const void* q, size_t n)
        {
            const auto* b = static_cast<const unsigned char*>(q);
            out.insert(out.end(), b, b + n);
        }
        void put_cstr(const char* s)
        {
            if (!s)
            {
                s = "";
            }
            const size_t n = std::strlen(s);
            put_out(s, n);
            const unsigned char nul = 0;
            out.push_back(nul);
            ret = (n != 0) ? 1 : 0;
        }
        void put_ret(const void* q, size_t n)
        {
            ret = 0;
            std::memcpy(&ret, q, n <= sizeof(ret) ? n : sizeof(ret));
        }
        void put_ret_value(uint64_t v)
        {
            ret = v;
        }
        template <typename T>
        void put_ret_floating(T v)
        {
            ret = 0;
            if constexpr (sizeof(T) == 4)
            {
                uint32_t b;
                std::memcpy(&b, &v, 4);
                ret = b;
            }
            else
            {
                std::memcpy(&ret, &v, sizeof(v));
            }
        }
    };

    // Clamp guest-influenced allocation sizes.
    inline size_t cap_bytes(size_t n)
    {
        return n > max_payload ? max_payload : n;
    }
    template <typename T>
    size_t cap_count(size_t n)
    {
        const size_t m = max_payload / (sizeof(T) ? sizeof(T) : 1);
        return n > m ? m : n;
    }

    // A method that marshals a struct self-stubs at compile time if that struct is only forward-declared.
    template <typename T, typename = void>
    struct is_complete : std::false_type
    {
    };
    template <typename T>
    struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type
    {
    };
    template <typename T>
    inline constexpr bool is_complete_v = is_complete<T>::value;

    inline void unsupported(const char*, const char*)
    {
    }

    // A method the security policy refuses to forward to the real Steam client (host filesystem access,
    // account-credential minting, arbitrary host-path/URL primitives). The guest gets steam_host_unsupported,
    // exactly as for an unimplemented method, so it fails closed. See the blocklist in the generator.
    inline void blocked(const char* iface, const char* method)
    {
        if (std::getenv("SOGEN_STEAM_TRACE"))
        {
            std::fprintf(stderr, "[steam-blocked] %s::%s denied by sandbox policy\n", iface, method);
            std::fflush(stderr);
        }
    }

    // Registers an interface pointer returned by a GetISteamXxx method and yields a handle the guest can
    // wrap in a proxy for `version`. Defined in the backend TU; called from the generated thunks.
    uint64_t register_returned_interface(const char* version, void* iface);

    // When a typed getter returns null (a modern host steamclient no longer vends a very old interface
    // version), obtain a newer version of the same family via GetISteamGenericInterface. Steam interfaces
    // are append-only, so the newer object is vtable-compatible for the requested version's method prefix.
    // Defined in the backend TU; must be called with the backend lock held (as the thunks are).
    void* host_resolve_fallback(const char* version);

}

#include "steam_reverse.hpp" // create_response_proxy(), referenced by the generated thunks

// Each version tag's thunks are included by its own generated TU (steam_host_tag.cpp.in), not here, so
// identically-named ISteam* classes from different SDK snapshots never share a translation unit.
