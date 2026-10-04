#include <gtest/gtest.h>

#include <module/macho_export_trie.hpp>

#include <map>
#include <string>
#include <vector>

namespace
{
    void append_uleb128(std::vector<uint8_t>& out, uint64_t value)
    {
        do
        {
            auto byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0)
            {
                byte |= 0x80;
            }
            out.push_back(byte);
        } while (value != 0);
    }

    // Builds the two-level trie a linker would emit for the given symbols: a root with one edge per
    // distinct first character is more than is needed here, so this emits a root whose children are the
    // whole remaining names. That is a legal trie and exercises edges, terminals and child offsets.
    std::vector<uint8_t> build_trie(const std::map<std::string, uint64_t>& symbols)
    {
        std::vector<std::vector<uint8_t>> leaves{};
        for (const auto& [name, address] : symbols)
        {
            std::vector<uint8_t> payload{};
            append_uleb128(payload, 0);       // flags
            append_uleb128(payload, address); // image offset

            std::vector<uint8_t> leaf{};
            append_uleb128(leaf, payload.size());
            leaf.insert(leaf.end(), payload.begin(), payload.end());
            leaf.push_back(0); // no children
            leaves.push_back(std::move(leaf));
        }

        // The root's size depends on the child offsets, which depend on the root's size. Solved by
        // iterating until it stops changing, which it does after one round.
        size_t root_size = 0;
        std::vector<uint8_t> root{};

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            root.clear();
            append_uleb128(root, 0); // no terminal at the root
            root.push_back(static_cast<uint8_t>(symbols.size()));

            auto child_offset = root_size;
            size_t index = 0;
            for (const auto& [name, address] : symbols)
            {
                root.insert(root.end(), name.begin(), name.end());
                root.push_back(0);
                append_uleb128(root, child_offset);
                child_offset += leaves[index].size();
                ++index;
            }

            if (root.size() == root_size)
            {
                break;
            }

            root_size = root.size();
        }

        for (const auto& leaf : leaves)
        {
            root.insert(root.end(), leaf.begin(), leaf.end());
        }

        return root;
    }

    TEST(MachoExportTrie, RecoversEverySymbolAndItsOffset)
    {
        const std::map<std::string, uint64_t> symbols{
            {"_CFRelease", 0x1234},
            {"_CFRetain", 0x2000},
            {"___CFInitialize", 0x17A0},
            {"_objc_msgSend", 0x400000},
        };

        const auto trie = build_trie(symbols);

        std::map<std::string, uint64_t> seen{};
        ASSERT_TRUE(
            sogen::walk_macho_export_trie(trie, [&](const std::string& name, const uint64_t offset, uint64_t) { seen[name] = offset; }));

        EXPECT_EQ(seen, symbols);
    }

    TEST(MachoExportTrie, RefusesWhatItCannotWalk)
    {
        EXPECT_FALSE(sogen::walk_macho_export_trie({}, [](const std::string&, uint64_t, uint64_t) {}));

        const std::vector<uint8_t> truncated{0x00, 0x01, 'a'};
        EXPECT_FALSE(sogen::walk_macho_export_trie(truncated, [](const std::string&, uint64_t, uint64_t) {}))
            << "an edge with no terminator runs off the end";

        // A child offset pointing at itself is a cycle. The bytes come from a cache the emulator did not
        // build, so this has to end rather than recurse until the stack does.
        std::vector<uint8_t> cyclic{};
        append_uleb128(cyclic, 0);
        cyclic.push_back(1);
        cyclic.push_back('a');
        cyclic.push_back(0);
        append_uleb128(cyclic, 0); // child offset 0: back to the root

        EXPECT_FALSE(sogen::walk_macho_export_trie(cyclic, [](const std::string&, uint64_t, uint64_t) {}));

        const std::vector<uint8_t> unterminated_number{0xFF, 0xFF, 0xFF};
        EXPECT_FALSE(sogen::walk_macho_export_trie(unterminated_number, [](const std::string&, uint64_t, uint64_t) {}))
            << "a uleb128 that never ends is not a number";
    }

    // A rejected trie must not have reported anything on the way to being rejected. The bytes come from a
    // cache the emulator did not build, and a caller told about a symbol from a trie that turned out to be
    // malformed has no way to know it should discard it.
    TEST(MachoExportTrie, AMalformedTrieReportsNothingBeforeItIsRefused)
    {
        // A terminal claiming 32 bytes in a buffer that has four. The flags and address that follow are
        // readable, so a walker that does not check the declared size first reports a symbol the trie
        // never really declared.
        const std::vector<uint8_t> oversized_terminal{0x20, 0x00, 0x40, 0x00};

        size_t reported = 0;
        EXPECT_FALSE(sogen::walk_macho_export_trie(oversized_terminal, [&](const std::string&, uint64_t, uint64_t) { ++reported; }));
        EXPECT_EQ(reported, 0u) << "a symbol was handed out of a trie that was then refused";
    }

    TEST(MachoExportTrie, NamesAreTheConcatenationOfTheEdgesWalked)
    {
        // Two symbols sharing a prefix, which is the whole point of a trie and the case a flat reader
        // gets wrong: the shared part appears once and each leaf carries only its own tail.
        std::vector<uint8_t> leaf_a{};
        append_uleb128(leaf_a, 2);
        append_uleb128(leaf_a, 0);
        append_uleb128(leaf_a, 0x10);
        leaf_a.push_back(0);

        std::vector<uint8_t> leaf_b{};
        append_uleb128(leaf_b, 2);
        append_uleb128(leaf_b, 0);
        append_uleb128(leaf_b, 0x20);
        leaf_b.push_back(0);

        std::vector<uint8_t> middle{};
        append_uleb128(middle, 0);
        middle.push_back(2);

        std::vector<uint8_t> trie{};
        append_uleb128(trie, 0);
        trie.push_back(1);
        trie.insert(trie.end(), {'_', 'C', 'F', 0});

        // Placeholder for the middle node's offset, patched once the layout is known.
        const auto patch_at = trie.size();
        append_uleb128(trie, 0x7F);

        const auto middle_offset = trie.size();
        trie[patch_at] = static_cast<uint8_t>(middle_offset);

        std::vector<uint8_t> children{};
        children.insert(children.end(), {'R', 'e', 't', 'a', 'i', 'n', 0});
        const auto leaf_a_at = middle_offset + middle.size() + 7 + 1 + 8 + 1;
        append_uleb128(children, leaf_a_at);
        children.insert(children.end(), {'R', 'e', 'l', 'e', 'a', 's', 'e', 0});
        append_uleb128(children, leaf_a_at + leaf_a.size());

        trie.insert(trie.end(), middle.begin(), middle.end());
        trie.insert(trie.end(), children.begin(), children.end());
        trie.resize(leaf_a_at, 0);
        trie.insert(trie.end(), leaf_a.begin(), leaf_a.end());
        trie.insert(trie.end(), leaf_b.begin(), leaf_b.end());

        std::map<std::string, uint64_t> seen{};
        sogen::walk_macho_export_trie(trie, [&](const std::string& name, const uint64_t offset, uint64_t) { seen[name] = offset; });

        EXPECT_TRUE(seen.contains("_CFRetain")) << "the shared prefix was not carried into the leaf";
        EXPECT_TRUE(seen.contains("_CFRelease"));
    }
}
