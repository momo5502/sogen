#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sogen
{

    namespace network
    {
        enum class network_mode : uint8_t
        {
            offline,
            live,
        };

        inline network_mode parse_network_mode(const std::string_view value)
        {
            if (value == "offline")
            {
                return network_mode::offline;
            }

            if (value == "live")
            {
                return network_mode::live;
            }

            throw std::invalid_argument("Network mode must be offline or live, not: " + std::string(value));
        }

        inline std::string_view to_string(const network_mode mode)
        {
            switch (mode)
            {
            case network_mode::offline:
                return "offline";
            case network_mode::live:
                return "live";
            }

            throw std::runtime_error("Unknown network mode");
        }
    }

} // namespace sogen
