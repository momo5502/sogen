#include <platform/compiler.hpp>

#ifdef OS_EMSCRIPTEN

#include <network/dns_lookup.hpp>
#include <network/socket_factory.hpp>
#include <utils/finally.hpp>
#include <gtest/gtest.h>
#include <emscripten.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace sogen::test
{
    namespace
    {
        // clang-format off
        EM_JS(void, install_http_response, (int head_response), {
            Module.sogenHttpSocketTestFetch = globalThis.fetch;
            globalThis.fetch = async function () {
                if (head_response) {
                    return new Response(null, {
                        status: 200,
                        headers: { "Content-Length": "12345", "Content-Encoding": "gzip" }
                    });
                }
                return new Response("response body", { status: 200 });
            };
        });

        EM_JS(void, restore_http_response, (), {
            globalThis.fetch = Module.sogenHttpSocketTestFetch;
            delete Module.sogenHttpSocketTestFetch;
        });
        // clang-format on

        network::address loopback_address(const uint16_t port)
        {
            network::address peer{};
            peer.set_ipv4(htonl(INADDR_LOOPBACK));
            peer.set_port(port);
            return peer;
        }

        int16_t poll_socket(network::socket_factory& factory, network::i_socket& socket, const int16_t events)
        {
            network::poll_entry entry{.s = &socket, .events = events, .revents = -1};
            const auto ready = factory.poll_sockets(std::span{&entry, 1});
            EXPECT_EQ(ready, entry.revents != 0 ? 1 : 0);
            return entry.revents;
        }

        void send_request(network::i_socket& socket, const std::string_view request)
        {
            EXPECT_EQ(socket.send(std::as_bytes(std::span(request))), static_cast<sent_size>(request.size()));
        }

        std::string read_response(network::i_socket& socket)
        {
            std::string response{};
            std::array<std::byte, 32> buffer{};
            for (;;)
            {
                const auto count = socket.recv(buffer);
                EXPECT_GE(count, 0);
                if (count <= 0)
                {
                    return response;
                }

                response.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(count));
            }
        }
    }

    TEST(EmscriptenHttpSocketTest, LiveDnsReturnsNonLoopbackAddressesAcceptedByTheBridge)
    {
        const auto lookup = network::create_dns_lookup(network::network_mode::live);
        const auto factory = network::create_socket_factory(network::network_mode::live);
        for (const auto family : {AF_INET, AF_INET6})
        {
            SCOPED_TRACE(family);
            const auto addresses = lookup->resolve_host("example.test", family);
            ASSERT_FALSE(addresses.empty());
            for (auto peer : addresses)
            {
                EXPECT_EQ(peer.get_family(), family);
                if (peer.is_ipv4())
                {
                    const auto ip = ntohl(peer.get_in_addr().sin_addr.s_addr);
                    EXPECT_NE(ip >> 24, 127u);
                    EXPECT_NE(ip, INADDR_ANY);
                }
                else
                {
                    EXPECT_FALSE(IN6_IS_ADDR_LOOPBACK(&peer.get_in6_addr().sin6_addr));
                    EXPECT_FALSE(IN6_IS_ADDR_UNSPECIFIED(&peer.get_in6_addr().sin6_addr));
                }
                peer.set_port(80);
                const auto socket = factory->create_socket(family, SOCK_STREAM, IPPROTO_TCP);
                EXPECT_TRUE(socket->connect(peer));
            }
        }

        const auto offline_lookup = network::create_dns_lookup(network::network_mode::offline);
        EXPECT_TRUE(offline_lookup->resolve_host("example.test", AF_INET).empty());
    }

    TEST(EmscriptenHttpSocketTest, LiveDnsHandlesManyDistinctHostnames)
    {
        const auto lookup = network::create_dns_lookup(network::network_mode::live);
        const auto factory = network::create_socket_factory(network::network_mode::live);
        for (size_t i = 0; i < 512; ++i)
        {
            const auto hostname = "host-" + std::to_string(i) + ".example.test";
            SCOPED_TRACE(hostname);
            for (const auto family : {AF_INET, AF_INET6})
            {
                SCOPED_TRACE(family);
                const auto addresses = lookup->resolve_host(hostname, family);
                ASSERT_FALSE(addresses.empty());
                for (auto peer : addresses)
                {
                    EXPECT_EQ(peer.get_family(), family);
                    peer.set_port(80);
                    const auto socket = factory->create_socket(family, SOCK_STREAM, IPPROTO_TCP);
                    EXPECT_TRUE(socket->connect(peer));
                }
            }
        }
    }

    TEST(EmscriptenHttpSocketTest, ReadabilityStartsWithTheResponseAndContinuesAtEndOfStream)
    {
        install_http_response(false);
        const auto cleanup = utils::finally([] { restore_http_response(); });
        const auto factory = network::create_socket_factory(network::network_mode::live);
        const auto socket = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        socket->set_blocking(false);

        constexpr auto read = static_cast<int16_t>(POLLIN | POLLRDNORM);
        constexpr auto write = static_cast<int16_t>(POLLOUT | POLLWRNORM);
        EXPECT_EQ(poll_socket(*factory, *socket, read | write), 0);
        ASSERT_TRUE(socket->connect(loopback_address(80)));
        EXPECT_TRUE(socket->is_ready(false));
        EXPECT_FALSE(socket->is_ready(true));
        EXPECT_EQ(poll_socket(*factory, *socket, read), 0);
        EXPECT_EQ(poll_socket(*factory, *socket, write), write);
        EXPECT_EQ(poll_socket(*factory, *socket, read | write), write);

        send_request(*socket, "GET / HTTP/1.1\r\nHost: example.test\r\n");
        EXPECT_EQ(poll_socket(*factory, *socket, read), 0);
        std::array<std::byte, 1> buffer{};
        EXPECT_EQ(socket->recv(buffer), -1);
        EXPECT_EQ(socket->get_last_error(), SERR(EWOULDBLOCK));

        send_request(*socket, "\r\n");
        EXPECT_TRUE(socket->is_ready(true));
        EXPECT_EQ(poll_socket(*factory, *socket, read | write), read | write);
        EXPECT_TRUE(read_response(*socket).ends_with("\r\n\r\nresponse body"));
        EXPECT_EQ(poll_socket(*factory, *socket, read), read);
        EXPECT_EQ(socket->recv(buffer), 0);
    }

    TEST(EmscriptenHttpSocketTest, DatagramReadabilityFollowsQueuedPacketsInMixedPolls)
    {
        const auto factory = network::create_socket_factory(network::network_mode::live);
        const auto receiver = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const auto sender = factory->create_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        receiver->set_blocking(false);
        sender->set_blocking(false);
        ASSERT_TRUE(receiver->bind(loopback_address(0)));
        ASSERT_TRUE(sender->bind(loopback_address(0)));
        constexpr auto read = static_cast<int16_t>(POLLRDNORM);
        EXPECT_EQ(poll_socket(*factory, *receiver, read), 0);

        const auto stream = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_TRUE(stream->connect(loopback_address(80)));
        const std::array packet{std::byte{1}, std::byte{2}, std::byte{3}};
        ASSERT_EQ(sender->sendto(*receiver->get_local_address(), packet), static_cast<sent_size>(packet.size()));
        std::array entries{
            network::poll_entry{.s = stream.get(), .events = read},
            network::poll_entry{.s = receiver.get(), .events = read},
            network::poll_entry{.s = nullptr, .events = read, .revents = -1},
        };
        EXPECT_EQ(factory->poll_sockets(entries), 1);
        EXPECT_EQ(entries[0].revents, 0);
        EXPECT_EQ(entries[1].revents, read);
        EXPECT_EQ(entries[2].revents, 0);

        network::address source{};
        std::array<std::byte, 3> received{};
        EXPECT_EQ(receiver->recvfrom(source, received), static_cast<sent_size>(packet.size()));
        EXPECT_EQ(received, packet);
        EXPECT_EQ(poll_socket(*factory, *receiver, read), 0);
    }

    TEST(EmscriptenHttpSocketTest, HeadKeepsTheRepresentationLengthWithoutReturningABody)
    {
        install_http_response(true);
        const auto cleanup = utils::finally([] { restore_http_response(); });
        const auto factory = network::create_socket_factory(network::network_mode::live);
        const auto socket = factory->create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_TRUE(socket->connect(loopback_address(80)));
        send_request(*socket, "HEAD / HTTP/1.1\r\nHost: example.test\r\n\r\n");
        auto response = read_response(*socket);
        std::ranges::transform(response, response.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        EXPECT_NE(response.find("\r\ncontent-length: 12345\r\n"), std::string::npos);
        EXPECT_NE(response.find("\r\ncontent-encoding: gzip\r\n"), std::string::npos);
        EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    }
}

#endif
