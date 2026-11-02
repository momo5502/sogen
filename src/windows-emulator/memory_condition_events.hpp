#pragma once

#include <array>
#include <string_view>

namespace sogen
{
    struct memory_condition_event
    {
        std::u16string_view name;
        bool signaled;
    };

    // Signal states Windows 11 reports while the system is under no memory pressure.
    inline constexpr std::array<memory_condition_event, 9> memory_condition_events{{
        {.name = u"\\KernelObjects\\LowMemoryCondition", .signaled = false},
        {.name = u"\\KernelObjects\\HighMemoryCondition", .signaled = true},
        {.name = u"\\KernelObjects\\LowPagedPoolCondition", .signaled = false},
        {.name = u"\\KernelObjects\\HighPagedPoolCondition", .signaled = true},
        {.name = u"\\KernelObjects\\LowNonPagedPoolCondition", .signaled = false},
        {.name = u"\\KernelObjects\\HighNonPagedPoolCondition", .signaled = true},
        {.name = u"\\KernelObjects\\LowCommitCondition", .signaled = true},
        {.name = u"\\KernelObjects\\HighCommitCondition", .signaled = false},
        {.name = u"\\KernelObjects\\MaximumCommitCondition", .signaled = false},
    }};
}
