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
}
