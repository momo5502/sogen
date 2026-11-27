#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr uint16_t listener_port = 28117;

        int16_t poll(network::socket_factory& factory, network::i_socket& socket, const int16_t events)
        {
            network::poll_entry entry{.s = &socket, .events = events};
            factory.poll_sockets(std::span{&entry, 1});
            return entry.revents;
        }

        std::string address_string(const std::optional<network::address>& address)
        {
            return address ? address->to_string() : "none";
        }
    }

    // AFD polls the in-process sockets with POLLIN and POLLOUT, which on POSIX are bits of their own rather than the
    // normal-data masks Windows defines them as.
    TEST(StaticSocketFactoryTest, AConnectedSocketReportsPollinAndPolloutReadiness)
    {
        const auto factory = network::create_static_socket_factory();
        const auto listener = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_TRUE(listener->bind(network::address{"127.0.0.1", listener_port}));
        ASSERT_TRUE(listener->listen(1));

        const auto client = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_TRUE(client->connect(network::address{"127.0.0.1", listener_port}));
        network::address peer{};
        const auto server = listener->accept(peer);
        ASSERT_NE(nullptr, server);

        EXPECT_EQ(POLLOUT, poll(*factory, *client, POLLIN | POLLOUT));

        constexpr std::string_view data = "SOGEN";
        ASSERT_EQ(static_cast<sent_size>(data.size()), server->send(std::as_bytes(std::span{data})));
        EXPECT_EQ(POLLIN, poll(*factory, *client, POLLIN));
    }

    TEST(StaticSocketFactoryTest, AConnectionFromTheUnspecifiedAddressHasLoopbackEnds)
    {
        const auto factory = network::create_static_socket_factory();
        const auto listener = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_TRUE(listener->bind(network::address{"0.0.0.0", listener_port}));
        ASSERT_TRUE(listener->listen(1));

        const auto client = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const auto client_port = client->get_local_address()->get_port();
        ASSERT_TRUE(client->connect(network::address{"127.0.0.7", listener_port}));
        network::address peer{};
        const auto server = listener->accept(peer);
        ASSERT_NE(nullptr, server);

        const auto client_end = network::address{"127.0.0.1", client_port}.to_string();
        EXPECT_EQ(client_end, address_string(client->get_local_address()));
        EXPECT_EQ(client_end, peer.to_string());
        EXPECT_EQ(client_end, address_string(server->get_remote_address()));
        EXPECT_EQ(network::address("127.0.0.7", listener_port).to_string(), address_string(server->get_local_address()));
    }

    TEST(StaticSocketFactoryTest, ADatagramFromTheUnspecifiedAddressComesFromLoopback)
    {
        const auto factory = network::create_static_socket_factory();
        const auto receiver = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        ASSERT_TRUE(receiver->bind(network::address{"127.0.0.1", listener_port}));

        const auto sender = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const auto sender_port = sender->get_local_address()->get_port();
        constexpr std::array data{std::byte{1}};
        ASSERT_EQ(static_cast<sent_size>(data.size()), sender->sendto(network::address{"127.0.0.1", listener_port}, data));

        std::array<std::byte, data.size()> buffer{};
        network::address source{};
        ASSERT_EQ(static_cast<sent_size>(data.size()), receiver->recvfrom(source, buffer));
        EXPECT_EQ(network::address("127.0.0.1", sender_port).to_string(), source.to_string());
        EXPECT_EQ(network::address("0.0.0.0", sender_port).to_string(), address_string(sender->get_local_address()));
    }
}
