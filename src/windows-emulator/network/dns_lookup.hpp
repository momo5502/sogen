#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <network/address.hpp>

namespace sogen
{

    namespace network
    {
        struct dns_lookup_failure
        {
            int status{};
            int wsa_error{};
            std::string message;
        };

        struct dns_lookup
        {
            dns_lookup();
            virtual ~dns_lookup() = default;

            virtual std::vector<address> resolve_host(std::string_view hostname, std::optional<int> family = std::nullopt,
                                                      std::optional<dns_lookup_failure>* failure = nullptr);
        };
    }

} // namespace sogen
