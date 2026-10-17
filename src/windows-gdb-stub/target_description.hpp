#pragma once

#include <string_view>

namespace sogen
{

    struct target_description
    {
        std::string_view name;
        std::string_view xml;
    };

} // namespace sogen
