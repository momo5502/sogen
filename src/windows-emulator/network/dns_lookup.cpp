#include "dns_lookup.hpp"

#include <cstdio>

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

        const auto hostname_string = std::string(hostname);
        addrinfo* result = nullptr;
        const auto status = getaddrinfo(hostname_string.c_str(), nullptr, &hints, &result);
        if (status != 0)
        {
#ifdef _WIN32
            std::fprintf(stderr, "dns_lookup::resolve_host failed: host=%s family=%d status=%d wsa_error=%d message=%s\n",
                         hostname_string.c_str(), family.value_or(AF_UNSPEC), status, WSAGetLastError(), gai_strerrorA(status));
#else
            std::fprintf(stderr, "dns_lookup::resolve_host failed: host=%s family=%d status=%d message=%s\n", hostname_string.c_str(),
                         family.value_or(AF_UNSPEC), status, gai_strerror(status));
#endif
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
