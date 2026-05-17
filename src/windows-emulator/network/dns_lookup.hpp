#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <network/address.hpp>

namespace network
{
    struct dns_lookup
    {
        dns_lookup();
        virtual ~dns_lookup() = default;

        virtual std::vector<address> resolve_host(std::string_view hostname, std::optional<int> family = std::nullopt);
    };
}
