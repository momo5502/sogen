#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace sogen
{

    namespace network
    {
        enum class socket_activity : uint8_t
        {
            connect,
            bind,
            sendto,
        };

        inline std::string_view to_string(const socket_activity activity)
        {
            switch (activity)
            {
            case socket_activity::connect:
                return "connect";
            case socket_activity::bind:
                return "bind";
            case socket_activity::sendto:
                return "sendto";
            }

            throw std::runtime_error("Unknown socket activity");
        }
    }

} // namespace sogen
