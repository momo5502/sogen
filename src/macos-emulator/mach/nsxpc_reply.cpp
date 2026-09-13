#include "../std_include.hpp"
#include "nsxpc_reply.hpp"

#include <algorithm>

namespace sogen::nsxpc
{
    namespace
    {
        // bplist17, the format NSXPC serializes an invocation and its reply into. Every tag is a byte;
        // the low nibble is a length for the inline kinds and a byte count for an integer. A collection
        // carries the ABSOLUTE offset of its own last byte, not its length. Measured 2026-08-28 against
        // lsd's replies to -[_LSDReadService getServerStoreNonBlockingWithCompletionHandler:] and against
        // the request LaunchServices sends it.
        constexpr uint8_t TAG_NIL = 0xE0;
        constexpr uint8_t TAG_FALSE = 0xB0;
        constexpr uint8_t TAG_INTEGER = 0x10;
        constexpr uint8_t TAG_ASCII = 0x70;
        constexpr uint8_t TAG_ARRAY = 0xA0;
        constexpr uint8_t TAG_LONG_FORM = 0x0F;

        class writer
        {
          public:
            writer()
            {
                constexpr std::string_view magic = "bplist17";
                bytes_.insert(bytes_.end(), magic.begin(), magic.end());
            }

            void append_nil()
            {
                bytes_.push_back(TAG_NIL);
            }

            void append_false()
            {
                bytes_.push_back(TAG_FALSE);
            }

            void append_integer(const uint64_t value)
            {
                size_t width = 1;
                while (width < 8 && (value >> (width * 8)) != 0)
                {
                    width *= 2;
                }

                bytes_.push_back(static_cast<uint8_t>(TAG_INTEGER | width));
                for (size_t i = 0; i < width; ++i)
                {
                    bytes_.push_back(static_cast<uint8_t>(value >> (i * 8)));
                }
            }

            void append_string(const std::string_view text)
            {
                const auto length = text.size() + 1;
                if (length <= TAG_LONG_FORM - 1)
                {
                    bytes_.push_back(static_cast<uint8_t>(TAG_ASCII | length));
                }
                else
                {
                    bytes_.push_back(TAG_ASCII | TAG_LONG_FORM);
                    append_integer(length);
                }

                bytes_.insert(bytes_.end(), text.begin(), text.end());
                bytes_.push_back(0);
            }

            size_t open_array()
            {
                bytes_.push_back(TAG_ARRAY);
                const auto placeholder = bytes_.size();
                bytes_.resize(bytes_.size() + sizeof(uint64_t), 0);
                return placeholder;
            }

            void close_array(const size_t placeholder)
            {
                const uint64_t last = bytes_.size() - 1;
                for (size_t i = 0; i < sizeof(uint64_t); ++i)
                {
                    bytes_[placeholder + i] = static_cast<uint8_t>(last >> (i * 8));
                }
            }

            std::vector<uint8_t> take()
            {
                return std::move(bytes_);
            }

          private:
            std::vector<uint8_t> bytes_{};
        };

        bool is_object_type(const std::string_view type)
        {
            return type.starts_with('@') || type == "#" || type == ":" || type == "*" || type.starts_with('^');
        }

        bool is_integer_type(const std::string_view type)
        {
            static constexpr std::string_view integers = "cCsSiIlLqQ";
            return type.size() == 1 && integers.find(type.front()) != std::string_view::npos;
        }
    }

    std::optional<std::string> invocation_selector(const std::vector<uint8_t>& root)
    {
        constexpr std::string_view magic = "bplist17";
        constexpr size_t array_header = 1 + sizeof(uint64_t);

        if (root.size() < magic.size() + array_header + 1 ||
            !std::equal(magic.begin(), magic.end(), reinterpret_cast<const char*>(root.data())) || root[magic.size()] != TAG_ARRAY)
        {
            return std::nullopt;
        }

        auto offset = magic.size() + array_header;
        const auto tag = root[offset++];
        if ((tag & 0xF0) != TAG_ASCII)
        {
            return std::nullopt;
        }

        size_t length = tag & 0x0F;
        if (length == TAG_LONG_FORM)
        {
            if (offset >= root.size() || (root[offset] & 0xF0) != TAG_INTEGER)
            {
                return std::nullopt;
            }

            const size_t width = root[offset++] & 0x0F;
            if (width == 0 || width > sizeof(uint64_t) || root.size() - offset < width)
            {
                return std::nullopt;
            }

            length = 0;
            for (size_t i = 0; i < width; ++i)
            {
                length |= static_cast<size_t>(root[offset + i]) << (i * 8);
            }

            offset += width;
        }

        if (length == 0 || root.size() - offset < length)
        {
            return std::nullopt;
        }

        return std::string{reinterpret_cast<const char*>(root.data() + offset), length - 1};
    }

    std::string strip_type_encoding_offsets(const std::string_view signature)
    {
        std::string stripped{};
        stripped.reserve(signature.size());

        for (size_t i = 0; i < signature.size();)
        {
            const auto c = signature[i];
            if (c >= '0' && c <= '9')
            {
                ++i;
                continue;
            }

            if (c == '"')
            {
                const auto end = signature.find('"', i + 1);
                if (end == std::string_view::npos)
                {
                    stripped.append(signature.substr(i));
                    break;
                }

                stripped.append(signature.substr(i, end - i + 1));
                i = end + 1;
                continue;
            }

            stripped.push_back(c);
            ++i;
        }

        return stripped;
    }

    std::optional<std::vector<std::string>> reply_argument_types(const std::string_view stripped_signature)
    {
        std::vector<std::string> types{};

        for (size_t i = 0; i < stripped_signature.size();)
        {
            const auto c = stripped_signature[i];

            if (c == '@' && i + 1 < stripped_signature.size() && stripped_signature[i + 1] == '?')
            {
                types.emplace_back("@?");
                i += 2;
                continue;
            }

            if (c == '@' && i + 1 < stripped_signature.size() && stripped_signature[i + 1] == '"')
            {
                const auto end = stripped_signature.find('"', i + 2);
                if (end == std::string_view::npos)
                {
                    return std::nullopt;
                }

                types.emplace_back(stripped_signature.substr(i, end - i + 1));
                i = end + 1;
                continue;
            }

            if (c == '^')
            {
                if (i + 1 >= stripped_signature.size())
                {
                    return std::nullopt;
                }

                types.emplace_back(stripped_signature.substr(i, 2));
                i += 2;
                continue;
            }

            types.emplace_back(1, c);
            ++i;
        }

        // A reply signature is always void, and its first argument is always the client's reply block.
        if (types.size() < 2 || types[0] != "v" || types[1] != "@?")
        {
            return std::nullopt;
        }

        return std::vector<std::string>{types.begin() + 2, types.end()};
    }

    std::optional<std::vector<uint8_t>> empty_reply_root(const std::string_view reply_signature, std::string* unsupported_type)
    {
        const auto stripped = strip_type_encoding_offsets(reply_signature);
        const auto arguments = reply_argument_types(stripped);
        if (!arguments.has_value())
        {
            if (unsupported_type != nullptr)
            {
                *unsupported_type = stripped;
            }

            return std::nullopt;
        }

        for (const auto& type : *arguments)
        {
            if (!is_object_type(type) && !is_integer_type(type) && type != "B")
            {
                if (unsupported_type != nullptr)
                {
                    *unsupported_type = type;
                }

                return std::nullopt;
            }
        }

        writer out{};
        const auto root = out.open_array();

        // Element 0 is the selector on a request and nil on a reply; element 1 is the signature without
        // its offsets; element 2 is the argument list, which excludes the reply block itself.
        out.append_nil();
        out.append_string(stripped);

        const auto args = out.open_array();
        for (const auto& type : *arguments)
        {
            if (type == "B")
            {
                out.append_false();
            }
            else if (is_integer_type(type))
            {
                out.append_integer(0);
            }
            else
            {
                out.append_nil();
            }
        }
        out.close_array(args);

        out.close_array(root);
        return out.take();
    }
}
