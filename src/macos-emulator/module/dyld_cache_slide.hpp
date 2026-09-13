#pragma once

#include "../std_include.hpp"

#include "../macos_emulator.hpp"

#include <functional>
#include <span>

namespace sogen
{
    // Rewrites the packed chain entries of a slid cache region into real pointers, signing the ones that
    // are authenticated so the guest's own autda accepts them.
    //
    // restrict_begin/restrict_end bound which pages are touched, which is what makes lazy paging
    // possible: the pager rebases a chunk as it materialises it rather than the whole 5.4 GB up front.
    // The default range covers everything, which is what the eager path passes.
    // How the slide blob itself is read. It lives in the cache's read-only region, which under lazy
    // paging is not resident -- reading it through guest memory would fault while handling a fault. The
    // pager supplies a reader that goes to the backing file instead; the eager path leaves it empty and
    // reads guest memory, where the blob is already mapped.
    using dyld_slide_metadata_reader = std::function<bool(uint64_t address, std::span<std::byte> destination)>;

    bool apply_dyld_cache_slide_info(macos_emulator& emu, uint64_t target, size_t length, uint64_t slide_start, uint64_t& applied,
                                     uint64_t restrict_begin = 0, uint64_t restrict_end = UINT64_MAX,
                                     const dyld_slide_metadata_reader& read_metadata = {});
}
