#include "dns_lookup.hpp"

#include <utils/finally.hpp>

#ifdef OS_EMSCRIPTEN
#include <emscripten.h>

#include <array>
#include <cstring>
#endif

namespace sogen
{

    namespace network
    {
        namespace
        {
#ifdef OS_EMSCRIPTEN
            EM_JS_DEPS(sogen_http_dns, "$DNS");

            // clang-format off
            EM_JS(int, sogen_http_lookup_host, (const char* hostname, char* destination, int length), {
                const resolved = DNS.lookup_name(UTF8ToString(Number(hostname)));
                return stringToUTF8(resolved, Number(destination), length);
            });
            // clang-format on

            struct http_dns_lookup final : dns_lookup
            {
                std::vector<address> resolve_host(const std::string_view hostname, const std::optional<int> family) override
                {
                    if (hostname.empty() || (family && *family != AF_UNSPEC && *family != AF_INET && *family != AF_INET6))
                    {
                        return {};
                    }

                    // fetch() resolves the Host authority; the guest only needs a synthetic address to connect.
                    const std::string name{hostname};
                    std::array<char, INET6_ADDRSTRLEN> text{};
                    if (!sogen_http_lookup_host(name.c_str(), text.data(), static_cast<int>(text.size())))
                    {
                        return {};
                    }

                    address resolved{};
                    in_addr ipv4{};
                    in6_addr ipv6{};
                    if (inet_pton(AF_INET, text.data(), &ipv4) == 1)
                    {
                        if (family && *family == AF_INET6)
                        {
                            ipv6.s6_addr[10] = 0xFF;
                            ipv6.s6_addr[11] = 0xFF;
                            std::memcpy(ipv6.s6_addr + 12, &ipv4, sizeof(ipv4));
                            resolved.set_ipv6(ipv6);
                        }
                        else
                        {
                            resolved.set_ipv4(ipv4);
                        }
                    }
                    else
                    {
                        if ((family && *family == AF_INET) || inet_pton(AF_INET6, text.data(), &ipv6) != 1)
                        {
                            return {};
                        }

                        resolved.set_ipv6(ipv6);
                    }

                    return {resolved};
                }
            };
#endif

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
            if (mode == network_mode::live)
            {
#ifdef OS_EMSCRIPTEN
                return std::make_unique<http_dns_lookup>();
#else
                return std::make_unique<dns_lookup>();
#endif
            }

            return std::make_unique<null_dns_lookup>();
        }
    }

} // namespace sogen
