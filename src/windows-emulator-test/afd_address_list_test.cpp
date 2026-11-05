#include "emulation_test_utils.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr ULONG ioctl_afd_address_list_query = 0x120B3;
        constexpr uint16_t win_af_inet = 2;
        constexpr uint16_t win_af_inet6 = 23;
        constexpr ULONG ipv4_entry_size = 18;
        constexpr std::byte untouched{0xCC};

        struct endpoint_creation_data
        {
            uint64_t flags{};
            std::array<char, 16> packet{};
            uint64_t group{};
            int32_t address_family{2};
            int32_t type{2};
            int32_t protocol{17};
        };

        struct local_address_socket_factory : network::socket_factory
        {
            std::unique_ptr<network::i_socket> create_socket(const int af, const int type, const int protocol) override
            {
                return this->inner->create_socket(af, type, protocol);
            }

            int poll_sockets(const std::span<network::poll_entry> entries) override
            {
                return this->inner->poll_sockets(entries);
            }

            std::vector<network::address> get_local_addresses(const int family) override
            {
                return family == AF_INET6 ? this->ipv6 : this->ipv4;
            }

            std::unique_ptr<network::socket_factory> inner = network::create_static_socket_factory();
            std::vector<network::address> ipv4{};
            std::vector<network::address> ipv6{};
        };

        network::address ipv6_address(const std::string_view text, const uint32_t scope_id)
        {
            network::address address{text, AF_INET6};
            address.get_in6_addr().sin6_scope_id = scope_id;
            return address;
        }

        // The query results were measured on Windows 11 by capturing mswsock's request for SIO_ADDRESS_LIST_QUERY. 32-bit
        // processes send and receive the same bytes.
        class AfdAddressListTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                auto address_factory = std::make_unique<local_address_socket_factory>();
                this->factory = address_factory.get();
                interfaces.socket_factory = std::move(address_factory);
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);

                const endpoint_creation_data creation{};
                this->emu->memory.write_memory(this->storage + creation_offset, &creation, sizeof(creation));
                this->endpoint = this->emu->process.devices.store(io_device_container{
                    u"Afd\\Endpoint", *this->emu, {.buffer = this->storage + creation_offset, .length = sizeof(creation)}});
            }

            NTSTATUS query(const uint16_t family, const ULONG output_length, const ULONG input_length = sizeof(uint16_t))
            {
                this->emu->memory.write_memory(this->storage + input_offset, family);
                const std::vector output_fill(output_capacity, untouched);
                this->emu->memory.write_memory(this->storage + output_offset, output_fill.data(), output_fill.size());

                io_device_context request{this->emu->memory};
                request.source_handle = this->endpoint;
                request.io_control_code = ioctl_afd_address_list_query;
                request.io_status_block = {this->emu->memory, this->storage + status_offset};
                request.input_buffer = this->storage + input_offset;
                request.input_buffer_length = input_length;
                request.output_buffer = this->storage + output_offset;
                request.output_buffer_length = output_length;
                return this->emu->process.devices.get(this->endpoint)->execute_ioctl(*this->emu, request);
            }

            uint64_t information() const
            {
                return this->emu->memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(this->storage + status_offset).Information;
            }

            std::vector<std::byte> output(const size_t length) const
            {
                std::vector<std::byte> bytes(length);
                this->emu->memory.read_memory(this->storage + output_offset, bytes.data(), bytes.size());
                return bytes;
            }

            template <size_t Size>
            static std::vector<std::byte> bytes(const std::array<uint8_t, Size>& values)
            {
                std::vector<std::byte> result(Size);
                memcpy(result.data(), values.data(), Size);
                return result;
            }

            static constexpr uint64_t creation_offset = 0x000;
            static constexpr uint64_t status_offset = 0x100;
            static constexpr uint64_t input_offset = 0x200;
            static constexpr uint64_t output_offset = 0x400;
            static constexpr size_t output_capacity = 0x400;

            std::unique_ptr<windows_emulator> emu{};
            local_address_socket_factory* factory{};
            uint64_t storage{};
            handle endpoint{};
        };
    }

    TEST_F(AfdAddressListTest, ListsTheIpv4AddressesAsTransportAddresses)
    {
        this->factory->ipv4 = {network::address{"172.31.32.1"}, network::address{"192.168.178.66"}};

        ASSERT_EQ(STATUS_SUCCESS, this->query(win_af_inet, 0x400));
        EXPECT_EQ(4u + (2 * ipv4_entry_size), this->information());
        EXPECT_EQ(bytes(std::to_array<uint8_t>({0x02, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x02, 0x00, 0x00, 0x00, 0xac, 0x1f, 0x20, 0x01,
                                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x02, 0x00, 0x00, 0x00,
                                                0xc0, 0xa8, 0xb2, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcc})),
                  this->output(4 + (2 * ipv4_entry_size) + 1));
    }

    TEST_F(AfdAddressListTest, ListsTheIpv6AddressesWithTheirScope)
    {
        this->factory->ipv6 = {ipv6_address("fe80::9e7d:3d3e:2889:6501", 22)};

        ASSERT_EQ(STATUS_SUCCESS, this->query(win_af_inet6, 0x400));
        EXPECT_EQ(34u, this->information());
        EXPECT_EQ(bytes(std::to_array<uint8_t>({0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9e, 0x7d,
                                                0x3d, 0x3e, 0x28, 0x89, 0x65, 0x01, 0x16, 0x00, 0x00, 0x00, 0xcc})),
                  this->output(35));
    }

    TEST_F(AfdAddressListTest, ShortOutputGetsTheCountAndTheWholeAddressesThatFit)
    {
        this->factory->ipv4 = {network::address{"172.31.32.1"}, network::address{"172.30.48.1"}, network::address{"192.168.17.1"}};
        constexpr ULONG required = 4 + (3 * ipv4_entry_size);

        ASSERT_EQ(STATUS_BUFFER_OVERFLOW, this->query(win_af_inet, 4 + ipv4_entry_size + ipv4_entry_size - 1));
        EXPECT_EQ(required, this->information());
        auto expected = bytes(std::to_array<uint8_t>({0x03, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x02, 0x00, 0x00, 0x00, 0xac,
                                                      0x1f, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        expected.resize(required, untouched);
        EXPECT_EQ(expected, this->output(required));

        ASSERT_EQ(STATUS_BUFFER_OVERFLOW, this->query(win_af_inet, sizeof(int32_t)));
        EXPECT_EQ(required, this->information());
        EXPECT_EQ(bytes(std::to_array<uint8_t>({0x03, 0x00, 0x00, 0x00, 0xcc})), this->output(5));
    }

    TEST_F(AfdAddressListTest, RejectsAnOutputWithoutRoomForTheCountOrAMissingFamily)
    {
        this->factory->ipv4 = {network::address{"192.168.178.66"}};

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(win_af_inet, sizeof(int32_t) - 1));
        EXPECT_EQ(0u, this->information());
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(win_af_inet, 0x400, sizeof(uint16_t) - 1));
        EXPECT_EQ(0u, this->information());
        EXPECT_EQ(bytes(std::to_array<uint8_t>({0xcc, 0xcc, 0xcc, 0xcc})), this->output(4));
    }

    TEST_F(AfdAddressListTest, OtherFamiliesHaveNoAddresses)
    {
        this->factory->ipv4 = {network::address{"192.168.178.66"}};
        this->factory->ipv6 = {ipv6_address("2a02:8071:9187:6c40:53d6:e57f:566e:e8c7", 0)};

        for (const uint16_t family : std::to_array<uint16_t>({0, 1}))
        {
            ASSERT_EQ(STATUS_SUCCESS, this->query(family, 0x400));
            EXPECT_EQ(sizeof(int32_t), this->information());
            EXPECT_EQ(bytes(std::to_array<uint8_t>({0x00, 0x00, 0x00, 0x00, 0xcc})), this->output(5));
        }
    }
}
