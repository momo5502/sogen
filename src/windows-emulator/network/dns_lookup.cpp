#include "dns_lookup.hpp"

#include <utils/finally.hpp>

namespace network
{
    dns_lookup::dns_lookup()
    {
        initialize_wsa();
    }

    std::vector<address> dns_lookup::resolve_host(const std::string_view hostname, const std::optional<int> family)
    {
        addrinfo hints{};
        if (family)
        {
            hints.ai_family = *family;
        }

        addrinfo* result = nullptr;
        if (getaddrinfo(std::string(hostname).c_str(), nullptr, &hints, &result) != 0)
        {
            return {};
        }

        const auto cleanup = utils::finally([&result] { freeaddrinfo(result); });

        std::vector<address> results{};
        for (const auto* current = result; current != nullptr; current = current->ai_next)
        {
            if (current->ai_family != AF_INET && current->ai_family != AF_INET6)
            {
                continue;
            }

            address resolved{};
            resolved.set_address(current->ai_addr, static_cast<socklen_t>(current->ai_addrlen));
            results.push_back(resolved);
        }

        return results;
    }
}
