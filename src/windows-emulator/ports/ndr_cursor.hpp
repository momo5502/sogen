#pragma once

#include "../port.hpp"

namespace sogen
{

    inline std::vector<uint8_t> read_rpc_request(const memory_interface& memory, const lpc_request_context& c)
    {
        std::vector<uint8_t> request(c.send_buffer_length);
        if (!request.empty() && !memory.try_read_memory(c.send_buffer, request.data(), request.size()))
        {
            request.clear();
        }

        return request;
    }

    struct ndr_cursor
    {
        std::span<const uint8_t> bytes{};
        size_t offset{};
        size_t pointer_size{8};

        bool remaining(const size_t count) const
        {
            return offset <= bytes.size() && (bytes.size() - offset) >= count;
        }

        bool align_to(const size_t alignment)
        {
            if (alignment == 0)
            {
                return false;
            }

            const auto misaligned = offset % alignment;
            if (misaligned == 0)
            {
                return true;
            }

            return skip(alignment - misaligned);
        }

        bool skip(const size_t count)
        {
            if (!remaining(count))
            {
                return false;
            }

            offset += count;
            return true;
        }

        bool read_u16(uint16_t& value)
        {
            if (!align_to(sizeof(uint16_t)) || !remaining(sizeof(uint16_t)))
            {
                return false;
            }

            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            offset += sizeof(value);
            return true;
        }

        bool read_u32(uint32_t& value)
        {
            if (!align_to(sizeof(uint32_t)) || !remaining(sizeof(uint32_t)))
            {
                return false;
            }

            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            offset += sizeof(value);
            return true;
        }

        bool read_count(size_t& value)
        {
            if (pointer_size == sizeof(uint32_t))
            {
                uint32_t narrowed{};
                if (!read_u32(narrowed))
                {
                    return false;
                }

                value = narrowed;
                return true;
            }

            if (!align_to(sizeof(uint64_t)) || !remaining(sizeof(uint64_t)))
            {
                return false;
            }

            uint64_t wide{};
            std::memcpy(&wide, bytes.data() + offset, sizeof(wide));
            offset += sizeof(wide);
            value = static_cast<size_t>(wide);
            return true;
        }

        bool read_pointer(uint64_t& value)
        {
            size_t raw{};
            if (!read_count(raw))
            {
                return false;
            }

            value = raw;
            return true;
        }

        bool read_bytes(const size_t count, std::vector<uint8_t>& out)
        {
            if (!remaining(count))
            {
                return false;
            }

            out.assign(bytes.data() + offset, bytes.data() + offset + count);
            offset += count;
            return true;
        }

        bool read_conformant_bytes(std::vector<uint8_t>& out)
        {
            size_t count{};
            if (!read_count(count))
            {
                return false;
            }

            return read_bytes(count, out);
        }

        bool read_unique_bytes(std::vector<uint8_t>& out)
        {
            uint64_t referent{};
            if (!read_pointer(referent))
            {
                return false;
            }

            if (referent == 0)
            {
                out.clear();
                return true;
            }

            return read_conformant_bytes(out);
        }

        bool read_ndr_string(std::u16string& out)
        {
            size_t max_chars{};
            size_t first{};
            size_t actual{};
            if (!read_count(max_chars) || !read_count(first) || !read_count(actual))
            {
                return false;
            }

            (void)max_chars;
            (void)first;
            // Bound the element count before scaling. actual is guest-controlled, and
            // actual * sizeof(char16_t) can wrap size_t so remaining() would pass.
            if (offset > bytes.size() || actual > (bytes.size() - offset) / sizeof(char16_t))
            {
                return false;
            }

            const auto bytes_count = actual * sizeof(char16_t);

            out.assign(reinterpret_cast<const char16_t*>(bytes.data() + offset), actual);
            offset += bytes_count;
            if (!out.empty() && out.back() == 0)
            {
                out.pop_back();
            }

            return true;
        }
    };

} // namespace sogen
