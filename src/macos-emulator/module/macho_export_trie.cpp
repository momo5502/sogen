#include "../std_include.hpp"
#include "macho_export_trie.hpp"

#include <optional>

namespace sogen
{
    namespace
    {
        // A trie deep enough to hit this is not one any linker produced; it is a cycle or a corruption.
        constexpr size_t MAX_TRIE_DEPTH = 128;

        std::optional<uint64_t> read_uleb128(const std::span<const uint8_t> data, size_t& offset)
        {
            uint64_t value = 0;
            uint32_t shift = 0;

            while (offset < data.size())
            {
                const auto byte = data[offset++];
                if (shift < 64)
                {
                    value |= static_cast<uint64_t>(byte & 0x7F) << shift;
                }

                shift += 7;

                if ((byte & 0x80) == 0)
                {
                    return value;
                }

                // 10 groups of 7 bits is already more than 64; anything longer is not a number.
                if (shift > 70)
                {
                    return std::nullopt;
                }
            }

            return std::nullopt;
        }

        bool walk_node(const std::span<const uint8_t> trie, const size_t node_offset, std::string& prefix, const size_t depth,
                       const macho_export_visitor& visit)
        {
            if (depth > MAX_TRIE_DEPTH || node_offset >= trie.size())
            {
                return false;
            }

            auto cursor = node_offset;

            const auto terminal_size = read_uleb128(trie, cursor);
            if (!terminal_size)
            {
                return false;
            }

            if (*terminal_size > trie.size() - cursor)
            {
                return false;
            }

            if (*terminal_size > 0)
            {
                auto terminal = cursor;

                const auto flags = read_uleb128(trie, terminal);
                const auto address = read_uleb128(trie, terminal);

                if (!flags || !address)
                {
                    return false;
                }

                visit(prefix, *address, *flags);
            }

            cursor += static_cast<size_t>(*terminal_size);
            if (cursor >= trie.size())
            {
                return false;
            }

            const auto child_count = trie[cursor++];

            for (uint8_t i = 0; i < child_count; ++i)
            {
                const auto edge_start = cursor;
                while (cursor < trie.size() && trie[cursor] != 0)
                {
                    ++cursor;
                }

                if (cursor >= trie.size())
                {
                    return false;
                }

                const auto edge = std::string{reinterpret_cast<const char*>(trie.data()) + edge_start, cursor - edge_start};
                ++cursor;

                const auto child = read_uleb128(trie, cursor);
                if (!child)
                {
                    return false;
                }

                const auto previous = prefix.size();
                prefix += edge;

                if (!walk_node(trie, static_cast<size_t>(*child), prefix, depth + 1, visit))
                {
                    return false;
                }

                prefix.resize(previous);
            }

            return true;
        }
    }

    bool walk_macho_export_trie(const std::span<const uint8_t> trie, const macho_export_visitor& visit)
    {
        if (trie.empty() || !visit)
        {
            return false;
        }

        std::string prefix{};
        return walk_node(trie, 0, prefix, 0, visit);
    }
}
