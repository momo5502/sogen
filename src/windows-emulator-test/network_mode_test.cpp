#include <network/dns_lookup.hpp>
#include <network/socket_factory.hpp>
#include <network/tcp_server_socket.hpp>
#include <platform/compiler.hpp>
#include <gtest/gtest.h>

#include <stdexcept>

namespace sogen::test
{
#ifndef OS_EMSCRIPTEN
    namespace
    {
        bool connect_to_host_listener(const network::network_mode mode)
        {
            network::tcp_server_socket listener{AF_INET};
            if (!listener.bind(network::address{"127.0.0.1", 0}))
            {
                throw std::runtime_error("Failed to bind host listener");
            }
            listener.listen();

            const auto factory = network::create_socket_factory(mode);
            const auto socket = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            return socket->connect(network::address{"127.0.0.1", listener.get_port()});
        }
    }

    TEST(NetworkModeTest, OfflineSocketCannotReachHostListener)
    {
        EXPECT_FALSE(connect_to_host_listener(network::network_mode::offline));
    }

    TEST(NetworkModeTest, LiveSocketReachesHostListener)
    {
        EXPECT_TRUE(connect_to_host_listener(network::network_mode::live));
    }
#endif

    TEST(NetworkModeTest, OfflineDnsResolvesNoAddresses)
    {
        const auto lookup = network::create_dns_lookup(network::network_mode::offline);
        EXPECT_TRUE(lookup->resolve_host("localhost").empty());
    }

    TEST(NetworkModeTest, ParseAcceptsOnlyKnownModes)
    {
        EXPECT_EQ(network::parse_network_mode("offline"), network::network_mode::offline);
        EXPECT_EQ(network::parse_network_mode("live"), network::network_mode::live);
        EXPECT_THROW(network::parse_network_mode("none"), std::runtime_error);
    }
}
