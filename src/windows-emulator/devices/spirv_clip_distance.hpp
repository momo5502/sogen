#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sogen
{
    // Rewrites a SPIR-V module so it no longer needs shaderClipDistance / shaderCullDistance: the built-in
    // variables become private ones, and built-in block members (gl_PerVertex) are dropped from their block with
    // their writes going to private variables instead. The capabilities go away. For drivers without those features,
    // whose compilers crash on such modules instead of rejecting them.
    //
    // Returns std::nullopt when the module uses neither built-in, or uses a stripped block in a way this pass does not
    // handle (other than through an output variable, or with the stripped members not at the end of the block).
    std::optional<std::vector<uint32_t>> strip_clip_and_cull_distance(std::span<const uint32_t> code, bool strip_clip, bool strip_cull);
}
