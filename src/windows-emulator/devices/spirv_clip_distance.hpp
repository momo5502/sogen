#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sogen
{
    // Rewrites a SPIR-V module so it no longer needs shaderClipDistance / shaderCullDistance: the built-in
    // variables become private ones, so writes to them are dropped and the capabilities go away. For drivers
    // without those features, whose compilers crash on such modules instead of rejecting them.
    //
    // Returns std::nullopt when the module uses neither built-in, or declares one as a block member, which this
    // pass does not handle.
    std::optional<std::vector<uint32_t>> strip_clip_and_cull_distance(std::span<const uint32_t> code, bool strip_clip, bool strip_cull);
}
