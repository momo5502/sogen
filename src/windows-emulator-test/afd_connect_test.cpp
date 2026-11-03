#include "emulation_test_utils.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr ULONG ioctl_afd_bind = 0x12003;
        constexpr ULONG ioctl_afd_start_listen = 0x1200B;
        constexpr ULONG ioctl_afd_super_connect = 0x120C7;
        constexpr ULONG ioctl_afd_get_remote_address = 0x1203F;
        constexpr ULONG ioctl_afd_set_context = 0x12047;

        constexpr ULONG socket_context_size = 168;
        constexpr ULONG super_connect_info_size = 26;
        constexpr ULONG aligned_connect_info_size = 28;

        constexpr uint16_t listener_port = 28971;
        constexpr uint16_t closed_port = 28972;

        struct endpoint_creation_data
        {
            uint64_t flags{};
            std::array<char, 16> packet{};
            uint64_t group{};
            int32_t address_family{2};
            int32_t type{1};
            int32_t protocol{6};
        };

        struct guest_sockaddr_in
        {
            int16_t family{};
            uint16_t port{};
            std::array<uint8_t, 4> address{};
            std::array<uint8_t, 8> zero{};
        };

        static_assert(sizeof(guest_sockaddr_in) == 16);

        guest_sockaddr_in loopback(const uint16_t port)
        {
            return {.family = 2, .port = htons(port), .address = {127, 0, 0, 1}};
        }

        struct send_limit
        {
            std::optional<size_t> remaining{};
        };

        struct limited_send_socket : network::i_socket
        {
            limited_send_socket(std::unique_ptr<network::i_socket> inner_socket, std::shared_ptr<send_limit> shared_limit)
                : inner(std::move(inner_socket)),
                  limit(std::move(shared_limit))
            {
            }

            void set_blocking(const bool blocking) override
            {
                this->inner->set_blocking(blocking);
            }

            int get_last_error() override
            {
                return this->would_block ? SERR(EWOULDBLOCK) : this->inner->get_last_error();
            }

            std::optional<int> get_socket_error() override
            {
                return this->inner->get_socket_error();
            }

            bool is_ready(const bool in_poll) override
            {
                return this->inner->is_ready(in_poll);
            }

            bool is_listening() override
            {
                return this->inner->is_listening();
            }

            std::optional<network::address> get_local_address() override
            {
                return this->inner->get_local_address();
            }

            std::optional<network::address> get_remote_address() override
            {
                return this->inner->get_remote_address();
            }

            bool bind(const network::address& addr) override
            {
                this->would_block = false;
                return this->inner->bind(addr);
            }

            bool connect(const network::address& addr) override
            {
                this->would_block = false;
                return this->inner->connect(addr);
            }

            bool listen(const int backlog) override
            {
                this->would_block = false;
                return this->inner->listen(backlog);
            }

            std::unique_ptr<network::i_socket> accept(network::address& peer) override
            {
                this->would_block = false;
                return this->inner->accept(peer);
            }

            sent_size send(const std::span<const std::byte> data) override
            {
                const auto allowed = std::min(data.size(), this->limit->remaining.value_or(data.size()));
                this->would_block = allowed == 0 && !data.empty();
                if (this->would_block)
                {
                    return -1;
                }

                const auto sent = this->inner->send(data.first(allowed));
                if (sent > 0 && this->limit->remaining)
                {
                    *this->limit->remaining -= static_cast<size_t>(sent);
                }

                return sent;
            }

            sent_size sendto(const network::address& destination, const std::span<const std::byte> data) override
            {
                this->would_block = false;
                return this->inner->sendto(destination, data);
            }

            sent_size recv(const std::span<std::byte> data) override
            {
                this->would_block = false;
                return this->inner->recv(data);
            }

            sent_size recvfrom(network::address& source, const std::span<std::byte> data) override
            {
                this->would_block = false;
                return this->inner->recvfrom(source, data);
            }

            std::unique_ptr<network::i_socket> inner{};
            std::shared_ptr<send_limit> limit{};
            bool would_block{};
        };

        struct limited_send_socket_factory : network::socket_factory
        {
            std::unique_ptr<network::i_socket> create_socket(const int af, const int type, const int protocol) override
            {
                return std::make_unique<limited_send_socket>(this->inner->create_socket(af, type, protocol), this->limit);
            }

            int poll_sockets(const std::span<network::poll_entry> entries) override
            {
                std::vector<network::poll_entry> inner_entries(entries.begin(), entries.end());
                for (auto& entry : inner_entries)
                {
                    if (auto* limited = dynamic_cast<limited_send_socket*>(entry.s))
                    {
                        entry.s = limited->inner.get();
                    }
                }

                this->inner->poll_sockets(inner_entries);

                constexpr auto write_events = static_cast<int16_t>(POLLOUT | POLLWRNORM);
                const bool can_send = this->limit->remaining.value_or(1) != 0;

                int ready_count = 0;
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    auto revents = static_cast<int16_t>(inner_entries[i].revents & ~write_events);
                    if (can_send)
                    {
                        revents = static_cast<int16_t>(revents | (entries[i].events & write_events));
                    }

                    entries[i].revents = revents;
                    ready_count += revents != 0 ? 1 : 0;
                }

                return ready_count;
            }

            std::unique_ptr<network::socket_factory> inner = network::create_static_socket_factory();
            std::shared_ptr<send_limit> limit = std::make_shared<send_limit>();
        };

        class AfdConnectTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                auto factory = std::make_unique<limited_send_socket_factory>();
                this->send_budget = factory->limit;
                interfaces.socket_factory = std::move(factory);
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->open_endpoint();

                this->listener = this->emu->socket_factory().create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                ASSERT_TRUE(this->listener->bind(network::address{"127.0.0.1", listener_port}));
                ASSERT_TRUE(this->listener->listen(1));
            }

            void open_endpoint()
            {
                const endpoint_creation_data creation{};
                this->emu->memory.write_memory(this->storage + creation_offset, &creation, sizeof(creation));
                this->endpoint = this->emu->process.devices.store(io_device_container{
                    u"Afd\\Endpoint", *this->emu, {.buffer = this->storage + creation_offset, .length = sizeof(creation)}});
            }

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status_block() const
            {
                return {this->emu->memory, this->storage + status_offset};
            }

            NTSTATUS ioctl(const ULONG code, const uint64_t input, const ULONG input_length, const uint64_t output,
                           const ULONG output_length)
            {
                io_device_context request{this->emu->memory};
                request.source_handle = this->endpoint;
                request.io_control_code = code;
                request.io_status_block = this->status_block();
                request.input_buffer = input;
                request.input_buffer_length = input_length;
                request.output_buffer = output;
                request.output_buffer_length = output_length;
                return this->emu->process.devices.get(this->endpoint)->execute_ioctl(*this->emu, request);
            }

            NTSTATUS bind()
            {
                std::array<std::byte, 20> info{};
                const guest_sockaddr_in any{.family = 2};
                memcpy(info.data() + 4, &any, sizeof(any));
                this->emu->memory.write_memory(this->storage + input_offset, info.data(), info.size());
                return this->ioctl(ioctl_afd_bind, this->storage + input_offset, static_cast<ULONG>(info.size()), 0, 0);
            }

            NTSTATUS listen()
            {
                const std::array<std::byte, 12> info{};
                this->emu->memory.write_memory(this->storage + input_offset, info.data(), info.size());
                return this->ioctl(ioctl_afd_start_listen, this->storage + input_offset, static_cast<ULONG>(info.size()), 0, 0);
            }

            NTSTATUS set_context()
            {
                const std::vector<std::byte> context(socket_context_size);
                this->emu->memory.write_memory(this->storage + context_offset, context.data(), context.size());
                return this->ioctl(ioctl_afd_set_context, this->storage + context_offset, socket_context_size, 0, 0);
            }

            static std::array<std::byte, 32> connect_info(const guest_sockaddr_in& target, const USHORT address_length = 14,
                                                          const LONG address_count = 1)
            {
                std::array<std::byte, 32> info{};
                memcpy(info.data() + 4, &address_count, sizeof(address_count));
                memcpy(info.data() + 8, &address_length, sizeof(address_length));
                memcpy(info.data() + 10, &target, sizeof(target));
                return info;
            }

            NTSTATUS super_connect(const guest_sockaddr_in& target, const std::string_view send_data = {},
                                   const ULONG input_length = super_connect_info_size, const USHORT address_length = 14,
                                   const LONG address_count = 1)
            {
                const auto info = connect_info(target, address_length, address_count);
                this->emu->memory.write_memory(this->storage + input_offset, info.data(), info.size());
                this->emu->memory.write_memory(this->storage + data_offset, send_data.data(), send_data.size());

                return this->ioctl(ioctl_afd_super_connect, this->storage + input_offset, input_length, this->storage + data_offset,
                                   static_cast<ULONG>(send_data.size()));
            }

            NTSTATUS super_connect_from(const uint64_t input, const ULONG declared_length)
            {
                const auto info = connect_info(loopback(listener_port));
                this->emu->memory.write_memory(input, info.data(), aligned_connect_info_size);

                return this->ioctl(ioctl_afd_super_connect, input, declared_length, 0, 0);
            }

            NTSTATUS query_remote_address(const ULONG output_length)
            {
                return this->ioctl(ioctl_afd_get_remote_address, 0, 0, this->storage + output_offset, output_length);
            }

            bool listener_has_connection() const
            {
                network::address peer{};
                return this->listener->accept(peer) != nullptr;
            }

            static constexpr uint64_t creation_offset = 0x000;
            static constexpr uint64_t status_offset = 0x100;
            static constexpr uint64_t input_offset = 0x200;
            static constexpr uint64_t data_offset = 0x300;
            static constexpr uint64_t output_offset = 0x400;
            static constexpr uint64_t context_offset = 0x500;

            std::unique_ptr<windows_emulator> emu{};
            std::unique_ptr<network::i_socket> listener{};
            std::shared_ptr<send_limit> send_budget{};
            handle endpoint{};
            uint64_t storage{};
        };
    }

    TEST_F(AfdConnectTest, SuperConnectSendsTheConnectDataAndReportsItsLength)
    {
        constexpr std::string_view request = "SOGEN-CONNECTEX";
        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        ASSERT_EQ(STATUS_SUCCESS, this->super_connect(loopback(listener_port), request));
        EXPECT_EQ(request.size(), this->status_block().read().Information);

        network::address peer{};
        const auto accepted = this->listener->accept(peer);
        ASSERT_NE(accepted, nullptr);

        std::array<std::byte, 64> received{};
        ASSERT_EQ(static_cast<sent_size>(request.size()), accepted->recv(received));
        EXPECT_EQ(0, memcmp(received.data(), request.data(), request.size()));
    }

    TEST_F(AfdConnectTest, SuperConnectCompletesOnlyOnceAllConnectDataIsSent)
    {
        constexpr std::string_view request = "SOGEN-CONNECTEX-SENT-IN-PARTS";
        ASSERT_EQ(STATUS_SUCCESS, this->bind());

        this->send_budget->remaining = 10;
        ASSERT_EQ(STATUS_PENDING, this->super_connect(loopback(listener_port), request));

        this->send_budget->remaining.reset();
        this->emu->process.devices.get(this->endpoint)->work(*this->emu);

        const auto block = this->status_block().read();
        EXPECT_EQ(STATUS_SUCCESS, block.Status);
        EXPECT_EQ(request.size(), block.Information);

        network::address peer{};
        const auto accepted = this->listener->accept(peer);
        ASSERT_NE(accepted, nullptr);

        std::array<std::byte, 64> received{};
        ASSERT_EQ(static_cast<sent_size>(request.size()), accepted->recv(received));
        EXPECT_EQ(0, memcmp(received.data(), request.data(), request.size()));
    }

    TEST_F(AfdConnectTest, RefusedSuperConnectStillReportsTheConnectDataLength)
    {
        constexpr std::string_view request = "SOGEN-CONNECTEX";
        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        EXPECT_EQ(STATUS_CONNECTION_REFUSED, this->super_connect(loopback(closed_port), request));
        EXPECT_EQ(request.size(), this->status_block().read().Information);
    }

    TEST_F(AfdConnectTest, SuperConnectOnAnUnboundSocketIsRejected)
    {
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->super_connect(loopback(listener_port)));
        EXPECT_FALSE(this->listener_has_connection());
    }

    TEST_F(AfdConnectTest, SuperConnectOnAConnectedSocketIsRejected)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        ASSERT_EQ(STATUS_SUCCESS, this->super_connect(loopback(listener_port)));
        EXPECT_EQ(STATUS_CONNECTION_ACTIVE, this->super_connect(loopback(listener_port)));
    }

    TEST_F(AfdConnectTest, SuperConnectOnAListeningSocketIsRejected)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        ASSERT_EQ(STATUS_SUCCESS, this->listen());
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->super_connect(loopback(listener_port)));
        EXPECT_FALSE(this->listener_has_connection());
    }

    TEST_F(AfdConnectTest, SuperConnectNeedsTheAddressTypeAndTheWholeSockaddr)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        for (const ULONG length : {0u, 4u, 9u, 10u, 11u})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->super_connect(loopback(listener_port), {}, length)) << length;
        }
        for (const ULONG length : {12u, 13u, 14u, 16u, 20u, 24u, 25u})
        {
            EXPECT_EQ(STATUS_INVALID_ADDRESS, this->super_connect(loopback(listener_port), {}, length)) << length;
        }
        EXPECT_FALSE(this->listener_has_connection());
    }

    TEST_F(AfdConnectTest, SuperConnectIgnoresTheAddressLengthAndCount)
    {
        struct request_shape
        {
            ULONG input_length;
            USHORT address_length;
            LONG address_count;
        };

        for (const auto& shape : {request_shape{.input_length = 26, .address_length = 14, .address_count = 1},
                                  request_shape{.input_length = 32, .address_length = 14, .address_count = 1},
                                  request_shape{.input_length = 26, .address_length = 0, .address_count = 1},
                                  request_shape{.input_length = 26, .address_length = 13, .address_count = 1},
                                  request_shape{.input_length = 26, .address_length = 28, .address_count = 1},
                                  request_shape{.input_length = 26, .address_length = 14, .address_count = 0},
                                  request_shape{.input_length = 26, .address_length = 14, .address_count = -1}})
        {
            this->open_endpoint();
            ASSERT_EQ(STATUS_SUCCESS, this->bind());
            EXPECT_EQ(STATUS_SUCCESS,
                      this->super_connect(loopback(listener_port), {}, shape.input_length, shape.address_length, shape.address_count))
                << shape.input_length << " " << shape.address_length << " " << shape.address_count;
            EXPECT_TRUE(this->listener_has_connection());
        }
    }

    TEST_F(AfdConnectTest, SuperConnectNeedsTheWholeDeclaredInputToBeReadable)
    {
        const auto region = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_TRUE(this->emu->memory.decommit_memory(region + 0x1000, 0x1000));
        const auto input = region + 0x1000 - aligned_connect_info_size;

        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->super_connect_from(input, 0x1D));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->super_connect_from(input, 0x1000));
        EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, this->super_connect_from(input, 0x100000));
        EXPECT_FALSE(this->listener_has_connection());

        EXPECT_EQ(STATUS_SUCCESS, this->super_connect_from(input, 0x1C));
        EXPECT_TRUE(this->listener_has_connection());
    }

    TEST_F(AfdConnectTest, SuperConnectIgnoresAReadableTailUpToTheInputLimit)
    {
        const auto input = this->emu->memory.allocate_memory(0x100000, memory_permission::read_write);

        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, this->super_connect_from(input, 0x18000));
        EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, this->super_connect_from(input, 0x100000));
        EXPECT_FALSE(this->listener_has_connection());

        for (const ULONG length : {0x1000u, 0x10001u})
        {
            this->open_endpoint();
            ASSERT_EQ(STATUS_SUCCESS, this->bind());
            EXPECT_EQ(STATUS_SUCCESS, this->super_connect_from(input, length)) << length;
            EXPECT_TRUE(this->listener_has_connection()) << length;
        }
    }

    TEST_F(AfdConnectTest, RemoteAddressIsThePeerSockaddrWithTheContextLength)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->set_context());
        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        ASSERT_EQ(STATUS_SUCCESS, this->super_connect(loopback(listener_port)));

        for (const ULONG length : {64u, 16u})
        {
            ASSERT_EQ(STATUS_SUCCESS, this->query_remote_address(length)) << length;
            EXPECT_EQ(socket_context_size, this->status_block().read().Information);

            const auto remote = this->emu->memory.read_memory<guest_sockaddr_in>(this->storage + output_offset);
            const auto expected = loopback(listener_port);
            EXPECT_EQ(expected.family, remote.family);
            EXPECT_EQ(expected.port, remote.port);
            EXPECT_EQ(expected.address, remote.address);
        }

        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query_remote_address(8));
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query_remote_address(0));
    }

    TEST_F(AfdConnectTest, RemoteAddressWithoutAPeerIsRejected)
    {
        EXPECT_EQ(STATUS_INVALID_CONNECTION, this->query_remote_address(64));
        ASSERT_EQ(STATUS_SUCCESS, this->bind());
        EXPECT_EQ(STATUS_INVALID_CONNECTION, this->query_remote_address(64));
        ASSERT_EQ(STATUS_SUCCESS, this->listen());
        EXPECT_EQ(STATUS_INVALID_CONNECTION, this->query_remote_address(64));
    }
}
