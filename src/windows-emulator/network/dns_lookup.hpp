#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <network/address.hpp>

#include "network_mode.hpp"

namespace sogen
{

    namespace network
    {
        struct dns_lookup
        {
            dns_lookup();
            virtual ~dns_lookup() = default;

            virtual std::vector<address> resolve_host(std::string_view hostname, std::optional<int> family = std::nullopt);
        };

        struct null_dns_lookup final : dns_lookup
        {
            std::vector<address> resolve_host(std::string_view hostname, std::optional<int> family = std::nullopt) override;
        };

        std::unique_ptr<dns_lookup> create_dns_lookup(network_mode mode);
    }

} // namespace sogen
