#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace sogen
{
    // The export trie is a prefix tree: each node holds an optional terminal payload and a list of edges,
    // each an edge string plus the offset of the child. A symbol's name is the concatenation of the edges
    // walked to reach it, which is why nothing here can be read without walking.
    //
    // Returns false if the trie is malformed. The bytes come from a shared cache the emulator did not
    // build, so every offset is checked against the buffer and the walk is depth-limited: a trie whose
    // child pointers form a cycle would otherwise never end.
    using macho_export_visitor = std::function<void(const std::string& name, uint64_t image_offset, uint64_t flags)>;

    bool walk_macho_export_trie(std::span<const uint8_t> trie, const macho_export_visitor& visit);
}
