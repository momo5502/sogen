#include "std_include.hpp"

#ifndef __EMSCRIPTEN__

#include <connection_handler.hpp>
#include <network/tcp_server_socket.hpp>

#include <future>

namespace sogen::test
{
    namespace
    {
        constexpr int small_socket_buffer = 4096;

        struct loopback_connection
        {
            network::tcp_client_socket stub{};
            network::tcp_client_socket client{AF_INET};
        };

        void limit_buffer(const network::socket& socket, const int option)
        {
            const int size = small_socket_buffer;
            EXPECT_EQ(0, setsockopt(socket.get_socket(), SOL_SOCKET, option, reinterpret_cast<const char*>(&size),
                                    static_cast<int>(sizeof(size))));
        }

        loopback_connection connect_on_loopback(const bool small_buffers = false)
        {
            network::tcp_server_socket server{AF_INET};
            EXPECT_TRUE(server.bind(network::address{"127.0.0.1", 0}));
            server.listen();

            loopback_connection connection{};
            if (small_buffers)
            {
                limit_buffer(connection.client, SO_RCVBUF);
            }

            EXPECT_TRUE(connection.client.connect(network::address{"127.0.0.1", server.get_port()}));
            connection.stub = server.accept();
            if (small_buffers)
            {
                limit_buffer(connection.stub, SO_SNDBUF);
            }

            return connection;
        }

        void fill_until_it_would_block(network::tcp_client_socket& socket)
        {
            ASSERT_TRUE(socket.set_blocking(false));
            const std::string chunk(small_socket_buffer, 'x');
            while (::send(socket.get_socket(), chunk.data(), static_cast<send_size>(chunk.size()), 0) > 0)
            {
            }

            EXPECT_EQ(SERR(EWOULDBLOCK), GET_SOCKET_ERROR());
        }

        std::string receive_until_closed(network::tcp_client_socket& socket)
        {
            std::string received{};
            while (const auto data = socket.receive())
            {
                received += *data;
            }

            return received;
        }
    }

    // The stub answers a detach with OK and then closes the connection; GDB waits for that OK.
    TEST(GdbConnectionHandlerTest, CloseSendsTheQueuedReplyFirst)
    {
        auto connection = connect_on_loopback();
        ASSERT_TRUE(connection.stub.is_valid());

        {
            gdb_stub::connection_handler handler{connection.stub};
            handler.send_reply("OK");
            handler.close();
        }

        EXPECT_EQ("$OK#9a", receive_until_closed(connection.client));
    }

    TEST(GdbConnectionHandlerTest, CloseReturnsWhileTheClientDoesNotRead)
    {
        auto connection = connect_on_loopback(true);
        ASSERT_TRUE(connection.stub.is_valid());
        fill_until_it_would_block(connection.stub);

        gdb_stub::connection_handler handler{connection.stub};
        handler.send_reply("OK");

        auto closed = std::async(std::launch::async, [&] { handler.close(); });
        const bool closed_in_time = closed.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
        if (!closed_in_time)
        {
            connection.client.close();
        }

        EXPECT_TRUE(closed_in_time);
    }
}
#endif
