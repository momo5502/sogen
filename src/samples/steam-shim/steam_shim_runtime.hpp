#pragma once

// Guest-side runtime the generated Steam proxies build on. Pulls in the real SDK headers (so the proxy
// classes derive from the true ISteam* interfaces and inherit their exact vtable layout) and provides
// the small marshalling helper each generated override uses. The actual transport (bridge_invoke) is
// implemented in steam_shim.cpp; this header is transport-agnostic so it can also be compiled in a
// standalone signature/ABI check without the emulator.

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

// Silence SDK deprecation pragmas that would trip /WX in a strict build of the generated proxies.
#include "steam_api.h"
#include "steam_gameserver.h"

namespace sogen::steam_shim
{
    // Implemented by the shim TU: pack (handle, method, in-blob) into an invoke IOCTL and return the
    // response (raw return value, plus an out-blob for string/out-param payloads).
    void bridge_invoke(uint64_t handle, uint32_t method, const void* in, uint32_t in_len, void* out, uint32_t out_cap, uint32_t* out_len,
                       uint64_t* ret);
    void report_unsupported(const char* iface, const char* method);

    // Maps an interface version string to the LATEST version of its family (defined by the latest-SDK
    // generated proxies). Old-SDK proxies use it to request the modern object from the host while still
    // handing the game the old vtable. Returns `requested` unchanged for an unknown family.
    const char* latest_version_for(const char* requested);

    // Resolves a guest proxy for a sub-interface returned by another interface's method (e.g. the object
    // ISteamClient::GetISteamUser hands back). Unlike a tag's local create_proxy this searches EVERY built
    // version tag: the interface versions a game mixes need not all originate from one SDK snapshot, so the
    // requested version may be owned by a different tag than the interface that returned it. In steam_shim.cpp.
    void* resolve_proxy(const char* version, uint64_t handle);

    // Registers a game-implemented response object (with its interface `type` = RESPONSE_IFACE_ID) and
    // returns an opaque token; reverse callbacks for that token are dispatched back to this object.
    uint64_t register_response_object(void* obj, int32_t type);

    // True only for a complete type (one whose sizeof is valid). Lets a method that marshals a struct
    // self-stub at compile time when that struct is only forward-declared under the build's macros.
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

    // Collects a call's in-arguments, performs the round-trip, and decodes the reply. One per call.
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
        // A by-value scalar always occupies a fixed 8-byte little-endian slot on the wire, so the format
        // is architecture-agnostic: a 32-bit guest and the 64-bit host agree even for size_t / pointer-
        // width types. Each side copies only its own native width to/from the low bytes.
        void put_scalar(const void* p, size_t n)
        {
            unsigned char slot[8] = {0};
            if (n > sizeof(slot))
            {
                n = sizeof(slot);
            }
            std::memcpy(slot, p, n);
            put(slot, sizeof(slot));
        }
        void put_cstr(const char* s)
        {
            if (!s)
            {
                s = "";
            }
            put(s, std::strlen(s) + 1);
        }
        // A game-implemented response object, sent as an opaque token + interface-type id so the host can
        // substitute a proxy that routes calls back to this object (reverse-callback channel).
        void put_callback_token(uint64_t token, int32_t type)
        {
            put(&token, sizeof(token));
            put(&type, sizeof(type));
        }
        // In struct/ref: a reference is always present; a pointer carries a presence byte first.
        void put_in_ref(const void* p, size_t n)
        {
            put(p, n);
        }
        void put_in_ptr(const void* p, size_t n)
        {
            const unsigned char present = p ? 1 : 0;
            put(&present, 1);
            if (p)
            {
                put(p, n);
            }
        }

        // Length-prefixed variable payload (in-array / in-buffer): a u32 length then the bytes, capped so
        // a bogus size can't blow up the request.
        void put_var(const void* p, size_t n)
        {
            if (n > max_payload)
            {
                n = max_payload;
            }
            const uint32_t len = static_cast<uint32_t>(n);
            put(&len, sizeof(len));
            if (p && len)
            {
                put(p, len);
            }
        }

        // Read the next `n` bytes from the sequential out-blob into a caller out-parameter.
        void get_out(void* dst, size_t n)
        {
            if (dst && out_pos_ + n <= out_len_)
            {
                std::memcpy(dst, out_.data() + out_pos_, n);
            }
            out_pos_ += n;
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
        // A string return travels in the out-blob (NUL-terminated) after any out-params, read from the cursor.
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
        static constexpr size_t max_payload = 16u << 20;            // 16 MiB cap per variable payload
        static constexpr size_t initial_out_capacity = 256u * 1024; // room for array/buffer replies
        uint64_t handle_;
        uint32_t method_;
        std::vector<unsigned char> in_{};
        std::vector<unsigned char> out_{};
        uint32_t out_len_ = 0;
        uint32_t out_pos_ = 0;
        uint64_t ret_ = 0;
    };
}
