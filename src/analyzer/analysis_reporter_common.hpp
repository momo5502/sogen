#pragma once

#include <type_traits>
#include <utility>

namespace analysis_reporter_detail
{
    template <typename... Ts>
    struct overloaded : Ts...
    {
        using Ts::operator()...;
    };

    template <typename... Ts>
    auto make_overloaded(Ts&&... ts)
    {
        return overloaded<std::decay_t<Ts>...>{std::forward<Ts>(ts)...};
    }
}
