#pragma once
#include <platform/platform.hpp>
#include <vector>
#include <cstddef>

namespace sogen::utils
{
    struct cached_signing_level
    {
        ULONG flags{};
        UCHAR level{};
        ULONG thumbprint_algorithm{};
        std::vector<std::byte> thumbprint{};
    };

    NTSTATUS query_cached_signing_level(int file_descriptor, cached_signing_level& result);
    NTSTATUS compare_signing_levels(UCHAR current_level, UCHAR required_level);
}
