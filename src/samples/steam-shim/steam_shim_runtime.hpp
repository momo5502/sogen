#pragma once

// Guest-side runtime the generated Steam proxies build on: the real SDK headers (so proxies inherit the
// exact ISteam* vtables) plus the marshalling helper each override uses. Transport is in steam_shim.cpp.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "steam_api.h"
#include "steam_gameserver.h"

#include "steam_bridge_protocol.hpp" // response_type: shared reverse-callback interface ids

namespace sogen::steam_shim
{
    // Packs (handle, method, in-blob) into an invoke IOCTL and returns the raw return plus an out-blob.
    void bridge_invoke(uint64_t handle, uint32_t method, const void* in, uint32_t in_len, void* out, uint32_t out_cap, uint32_t* out_len,
                       uint64_t* ret);
    void report_unsupported(const char* iface, const char* method);

    // The latest version of `requested`'s interface family. Old-SDK proxies request the modern host object
    // while still handing the game the old vtable. Returns `requested` unchanged for an unknown family.
    const char* latest_version_for(const char* requested);

    // Resolves a guest proxy for a sub-interface handle across EVERY built tag: a game mixes interface
    // versions from more than one SDK snapshot, so the version may be owned by a different tag.
    void* resolve_proxy(const char* version, uint64_t handle);

    // Registers a game-implemented response object (interface `type` = steam_bridge::response_type); reverse
    // callbacks for the returned token are dispatched back to it.
    uint64_t register_response_object(void* obj, int32_t type);

    // Lets a method that marshals a struct self-stub at compile time when the struct is forward-declared.
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

    // Collects a call's in-arguments, does the round-trip, and decodes the reply.
    struct invoker
    {
        invoker(uint64_t handle, uint32_t method)
            : handle_(handle),
              method_(method)
        {
            out_.resize(initial_out_capacity);
        }

        void put(const void* p, size_t n)
        {
            const auto* b = static_cast<const unsigned char*>(p);
            in_.insert(in_.end(), b, b + n);
        }

        // Every by-value scalar takes a fixed 8-byte slot, so the wire format is arch-agnostic (a 32-bit
        // guest and the 64-bit host agree); each side copies only its native width to/from the low bytes.
        void put_scalar(const void* p, size_t n)
        {
            std::array<unsigned char, 8> slot{};
            n = (std::min)(n, slot.size());
            std::memcpy(slot.data(), p, n);
            put(slot.data(), slot.size());
        }

        void put_cstr(const char* s)
        {
            if (!s)
            {
                s = "";
            }
            put(s, std::strlen(s) + 1);
        }

        // Opaque token + interface-type id for a game response object; the host substitutes a proxy that
        // routes calls back (reverse-callback channel).
        void put_callback_token(uint64_t token, int32_t type)
        {
            put(&token, sizeof(token));
            put(&type, sizeof(type));
        }

        void put_in_ref(const void* p, size_t n)
        {
            put(p, n);
        }

        // A pointer arg carries a presence byte first; a reference is always present.
        void put_in_ptr(const void* p, size_t n)
        {
            const unsigned char present = p ? 1 : 0;
            put(&present, 1);
            if (p)
            {
                put(p, n);
            }
        }

        // Length-prefixed variable payload (in-array / in-buffer), capped so a bogus size can't blow up the request.
        void put_var(const void* p, size_t n)
        {
            n = (std::min)(n, max_payload);
            const auto len = static_cast<uint32_t>(n);
            put(&len, sizeof(len));
            if (p && len)
            {
                put(p, len);
            }
        }

        void get_out(void* dst, size_t n)
        {
            if (dst && out_pos_ + n <= out_len_)
            {
                std::memcpy(dst, out_.data() + out_pos_, n);
            }
            out_pos_ += static_cast<uint32_t>(n);
        }

        void call()
        {
            bridge_invoke(handle_, method_, in_.data(), static_cast<uint32_t>(in_.size()), out_.data(), static_cast<uint32_t>(out_.size()),
                          &out_len_, &ret_);
        }

        uint64_t ret_value() const
        {
            return ret_;
        }

        void ret_bytes(void* dst, size_t n)
        {
            std::memcpy(dst, &ret_, n <= sizeof(ret_) ? n : sizeof(ret_));
        }

        template <typename T>
        T ret_floating()
        {
            T v{};
            if constexpr (sizeof(T) == 4)
            {
                const auto bits = static_cast<uint32_t>(ret_);
                std::memcpy(&v, &bits, 4);
            }
            else
            {
                std::memcpy(&v, &ret_, sizeof(v));
            }
            return v;
        }

        // A string return follows any out-params in the out-blob, NUL-terminated.
        const char* ret_cstr()
        {
            static thread_local std::vector<char> buf;
            buf.clear();

            while (out_pos_ < out_len_ && out_[out_pos_] != '\0')
            {
                buf.push_back(static_cast<char>(out_[out_pos_++]));
            }
            buf.push_back('\0');
            return buf.data();
        }

      private:
        static constexpr size_t max_payload = 16u << 20;
        static constexpr size_t initial_out_capacity = 256u * 1024;
        uint64_t handle_;
        uint32_t method_;
        std::vector<unsigned char> in_{};
        std::vector<unsigned char> out_{};
        uint32_t out_len_ = 0;
        uint32_t out_pos_ = 0;
        uint64_t ret_ = 0;
    };
}
