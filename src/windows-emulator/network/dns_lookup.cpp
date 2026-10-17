#include "dns_lookup.hpp"

#include <utils/finally.hpp>

namespace sogen
{

    namespace network
    {
        namespace
        {
            struct null_dns_lookup final : dns_lookup
            {
                std::vector<address> resolve_host(const std::string_view /*hostname*/, const std::optional<int> /*family*/) override
                {
                    return {};
                }
            };
        }

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

        std::unique_ptr<dns_lookup> create_dns_lookup(const network_mode mode)
        {
            // Guest sockets on Emscripten are always in-process, so host DNS results would be unreachable.
#ifndef OS_EMSCRIPTEN
            if (mode == network_mode::live)
            {
                return std::make_unique<dns_lookup>();
            }
#endif

            (void)mode;
            return std::make_unique<null_dns_lookup>();
        }
    }

} // namespace sogen
