#pragma once

// Runtime support for the generated Vulkan struct (de)serialization (see
// tools/vulkan-bridge-generator/generate.py). `writer` flattens values into a byte stream on the
// guest; `reader` walks the stream on the host; `arena` owns the memory for pointees rebuilt during
// decode and must outlive the host Vulkan call. The generated encode/decode call the helpers here.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace sogen::gpu_bridge::marshal
{
    class writer
    {
      public:
        explicit writer(std::vector<std::byte>& out)
            : out_(out)
        {
        }

        void bytes(const void* data, size_t size)
        {
            const auto* begin = static_cast<const std::byte*>(data);
            this->out_.insert(this->out_.end(), begin, begin + size);
        }

        template <typename T>
        void scalar(const T& value)
        {
            this->bytes(&value, sizeof(T));
        }

        void flag(bool present)
        {
            this->scalar(static_cast<uint8_t>(present ? 1 : 0));
        }

      private:
        std::vector<std::byte>& out_;
    };

    class reader
    {
      public:
        reader(const void* data, size_t size)
            : cur_(static_cast<const std::byte*>(data)),
              end_(static_cast<const std::byte*>(data) + size)
        {
        }

        bool bytes(void* out, size_t size)
        {
            if (static_cast<size_t>(this->end_ - this->cur_) < size)
            {
                return false;
            }
            std::memcpy(out, this->cur_, size);
            this->cur_ += size;
            return true;
        }

        template <typename T>
        bool scalar(T& value)
        {
            return this->bytes(&value, sizeof(T));
        }

        bool flag(bool& present)
        {
            uint8_t raw = 0;
            if (!this->scalar(raw))
            {
                return false;
            }
            present = raw != 0;
            return true;
        }

      private:
        const std::byte* cur_;
        const std::byte* end_;
    };

    // Owns the memory backing decoded pointees (strings, nested structs, arrays). Lives for the
    // duration of the host call that consumes the decoded structures.
    class arena
    {
      public:
        void* allocate(size_t size)
        {
            if (size == 0)
            {
                return nullptr;
            }
            // Moving a std::vector preserves its heap buffer, so pointers stay valid even when the
            // outer vector reallocates.
            this->blocks_.emplace_back(size);
            return this->blocks_.back().data();
        }

        template <typename T>
        T* allocate_array(size_t count)
        {
            return static_cast<T*>(this->allocate(count * sizeof(T)));
        }

      private:
        std::vector<std::vector<std::byte>> blocks_;
    };

    inline void encode_string(writer& w, const char* str)
    {
        if (!str)
        {
            w.flag(false);
            return;
        }
        w.flag(true);
        const auto length = static_cast<uint32_t>(std::strlen(str));
        w.scalar(length);
        w.bytes(str, length);
    }

    inline const char* decode_string(reader& r, arena& a)
    {
        bool present = false;
        if (!r.flag(present) || !present)
        {
            return nullptr;
        }
        uint32_t length = 0;
        if (!r.scalar(length))
        {
            return nullptr;
        }
        char* str = a.allocate_array<char>(static_cast<size_t>(length) + 1);
        if (!r.bytes(str, length))
        {
            return nullptr;
        }
        str[length] = '\0';
        return str;
    }

    inline void encode_string_array(writer& w, const char* const* strings, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            encode_string(w, strings ? strings[i] : nullptr);
        }
    }

    inline const char* const* decode_string_array(reader& r, arena& a, uint32_t count)
    {
        if (count == 0)
        {
            return nullptr;
        }
        char** strings = a.allocate_array<char*>(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            strings[i] = const_cast<char*>(decode_string(r, a));
        }
        return strings;
    }

    // pNext is not yet remoted: encode drops the chain and decode yields nullptr. Extension-struct
    // support (sType-dispatched chain walking) will replace these as the surface grows.
    inline void encode_pnext(writer& w, const void* /*pNext*/)
    {
        w.flag(false);
    }

    inline bool decode_pnext(reader& r)
    {
        bool present = false;
        return r.flag(present);
    }
}

// The generator emits concrete encode/decode overloads (one per allowlisted struct) into the
// generated header, with inline handling of nested struct pointers, so all cross-struct calls are
// ordinary (non-dependent) overload resolution against the forward declarations -- no ADL needed.
