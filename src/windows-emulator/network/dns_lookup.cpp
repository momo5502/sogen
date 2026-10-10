#include "dns_lookup.hpp"

#include <utils/finally.hpp>

namespace sogen
{

    namespace network
    {
        dns_lookup::dns_lookup()
        {
            initialize_wsa();
        }

        std::vector<address> dns_lookup::resolve_host(const std::string_view hostname, const std::optional<int> family,
                                                      std::optional<dns_lookup_failure>* failure)
        {
            if (failure)
            {
                failure->reset();
            }
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
                if (failure)
                {
                    auto& error = failure->emplace();
                    error.status = status;
#ifdef _WIN32
                    error.wsa_error = WSAGetLastError();
                    error.message = gai_strerrorA(status);
#else
                    error.message = gai_strerror(status);
#endif
                }
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

} // namespace sogen
