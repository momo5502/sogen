#include <gtest/gtest.h>

#include <network/address.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace sogen
{
    TEST(network_address_test, parses_bind_endpoint_with_explicit_family)
    {
        const auto* bind_address = "127.0.0.1:28960";
        const network::address address{std::string_view{bind_address}, std::optional<int>{AF_INET}};

        EXPECT_TRUE(address.is_ipv4());
        EXPECT_EQ(AF_INET, address.get_family());
        EXPECT_EQ(28960, address.get_port());
        EXPECT_EQ("127.0.0.1:28960", address.to_string());
    }

    TEST(network_address_test, keeps_plain_numeric_pair_as_host_port)
    {
        const network::address address{std::string{"127.0.0.1"}, uint16_t{80}};

        EXPECT_TRUE(address.is_ipv4());
        EXPECT_EQ(AF_INET, address.get_family());
        EXPECT_EQ(80, address.get_port());
        EXPECT_EQ("127.0.0.1:80", address.to_string());
    }
}
