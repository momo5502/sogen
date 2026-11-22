#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sogen
{
    struct pdb_signature
    {
        std::string guid{};
        uint32_t age{};
        std::string path{};

        bool valid() const
        {
            return !this->guid.empty() && this->age != 0;
        }

        std::string_view filename() const
        {
            const auto separator = this->path.find_last_of("/\\");
            return std::string_view{this->path}.substr(separator == std::string::npos ? 0 : separator + 1);
        }

        bool has_safe_filename() const
        {
            const auto name = this->filename();
            return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string_view::npos;
        }
    };
}
