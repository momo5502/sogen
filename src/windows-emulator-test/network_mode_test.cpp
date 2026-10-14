#include <network/dns_lookup.hpp>
#include <network/socket_factory.hpp>
#include <network/tcp_server_socket.hpp>
#include <platform/compiler.hpp>
#include <gtest/gtest.h>

#include <array>
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

    TEST(NetworkModeTest, OfflineLoopbackClientReachesWildcardListener)
    {
        const auto factory = network::create_socket_factory(network::network_mode::offline);
        const auto server = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_TRUE(server->bind(network::address{"0.0.0.0", 28970}));
        ASSERT_TRUE(server->listen(1));

        const auto client = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        EXPECT_TRUE(client->connect(network::address{"127.0.0.1", 28970}));

        network::address peer{};
        EXPECT_NE(server->accept(peer), nullptr);
    }

    TEST(NetworkModeTest, OfflineLoopbackDatagramReachesWildcardReceiver)
    {
        const auto factory = network::create_socket_factory(network::network_mode::offline);
        const auto receiver = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        ASSERT_TRUE(receiver->bind(network::address{"0.0.0.0", 28970}));

        const auto sender = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const std::array data{std::byte{1}, std::byte{2}, std::byte{3}};
        ASSERT_EQ(sender->sendto(network::address{"127.0.0.1", 28970}, data), static_cast<sent_size>(data.size()));

        std::array<std::byte, 8> buffer{};
        network::address source{};
        EXPECT_EQ(receiver->recvfrom(source, buffer), static_cast<sent_size>(data.size()));
    }

    TEST(NetworkModeTest, OfflineDnsResolvesNoAddresses)
    {
        const auto lookup = network::create_dns_lookup(network::network_mode::offline);
        EXPECT_TRUE(lookup->resolve_host("localhost").empty());
    }

    TEST(NetworkModeTest, ParseAcceptsOnlyKnownModes)
    {
        EXPECT_EQ(network::parse_network_mode("offline"), network::network_mode::offline);
        EXPECT_EQ(network::parse_network_mode("live"), network::network_mode::live);
        EXPECT_THROW(network::parse_network_mode("none"), std::invalid_argument);
    }
}
