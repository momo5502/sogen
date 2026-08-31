#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <memory_interface.hpp>

#include "../macos_emulator_callbacks.hpp"

namespace sogen
{
    struct macos_trace_options
    {
        size_t string_limit{256};
        size_t buffer_preview_limit{32};
    };

    std::vector<macos_trace_detail> describe_bsd_syscall(const memory_interface& memory, uint32_t number,
                                                         std::span<const uint64_t> arguments, const macos_trace_options& options);

    // No memory and no options, and the absence is the point: XNU records a name and an argument count
    // for a mach trap and nothing else -- no argument names, no types -- so there is nothing to
    // dereference and nothing to configure. This cannot fault.
    std::vector<macos_trace_detail> describe_mach_trap(uint32_t index, std::span<const uint64_t> arguments);
}
