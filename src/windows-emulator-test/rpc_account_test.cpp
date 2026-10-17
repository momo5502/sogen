#include "emulation_test_utils.hpp"
#include <port.hpp>
#include <registry/registry_utils.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> sspi_interface{0xc8, 0xad, 0x32, 0x4f, 0x52, 0x60, 0x04, 0x4a,
                                                         0x87, 0x01, 0x29, 0x3c, 0xcf, 0x20, 0x96, 0xf0};
        constexpr std::array<uint8_t, 16> lsa_lookup_interface{0x29, 0x07, 0x8a, 0xfb, 0x04, 0x2d, 0x58, 0x46,
                                                               0xbe, 0x93, 0x27, 0xb4, 0xad, 0x55, 0x3f, 0xac};
        constexpr std::array<uint8_t, 16> lsarpc_interface{0x78, 0x57, 0x34, 0x12, 0x34, 0x12, 0xcd, 0xab,
                                                           0xef, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab};

        class RpcAccountTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->emu->process.sid = {1, 5, 0, 0, 0, 0, 0, 5};
                constexpr std::array<uint32_t, 5> authorities{21, 123, 456, 789, 1001};
                const auto* bytes = reinterpret_cast<const uint8_t*>(authorities.data());
                this->emu->process.sid.insert(this->emu->process.sid.end(), bytes, bytes + sizeof(authorities));
            }

            void bind(std::u16string_view name, const std::array<uint8_t, 16>& interface_id)
            {
                this->port = create_port(name);
                this->emu->memory.set_memory(this->storage, 0, 0x1000);
                this->emu->memory.write_memory(this->storage, uint32_t{1});
                this->emu->memory.write_memory(this->storage + 12, interface_id.data(), interface_id.size());
                this->emu->memory.write_memory(this->storage + 32,
                                               uint16_t{this->emu->process.is_wow64_process ? uint16_t{1} : uint16_t{3}});
                const lpc_request_context request{.send_buffer = this->storage,
                                                  .send_buffer_length = 0x48,
                                                  .recv_buffer = this->storage + 0x800,
                                                  .recv_buffer_length = 0x800};
                const auto reply = this->port->handle_request(*this->emu, request);
                ASSERT_EQ(STATUS_SUCCESS, reply.status);
                ASSERT_TRUE(reply.payload.has_value());
                EXPECT_EQ(1u, read<uint32_t>(*reply.payload, 0));
                ASSERT_GE(reply.payload->size(), 12 + interface_id.size());
                EXPECT_EQ(0, std::memcmp(reply.payload->data() + 12, interface_id.data(), interface_id.size()));
            }

            lpc_request_result call(uint32_t procedure, std::span<const uint8_t> data)
            {
                this->emu->memory.set_memory(this->storage, 0, 0x800);
                this->emu->memory.write_memory(this->storage + 12, uint32_t{123});
                this->emu->memory.write_memory(this->storage + 20, procedure);
                if (!data.empty())
                {
                    this->emu->memory.write_memory(this->storage + 0x40, data.data(), data.size());
                }
                const lpc_request_context request{.send_buffer = this->storage,
                                                  .send_buffer_length = static_cast<ULONG>(0x40 + data.size()),
                                                  .recv_buffer = this->storage + 0x800,
                                                  .recv_buffer_length = 0x800};
                return this->port->handle_request(*this->emu, request);
            }

            template <typename T>
            static T read(const std::vector<uint8_t>& bytes, size_t offset)
            {
                T value{};
                if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
                {
                    ADD_FAILURE() << "RPC response is truncated";
                    return value;
                }
                std::memcpy(&value, bytes.data() + offset, sizeof(value));
                return value;
            }

            std::unique_ptr<windows_emulator> emu{};
            std::unique_ptr<sogen::port> port{};
            uint64_t storage{};
        };
    }

    TEST_F(RpcAccountTest, SspiNamesIncludeExtendedErrorAndReturnedStatus)
    {
        this->bind(u"\\RPC Control\\lsasspirpc", sspi_interface);
        for (const auto format : {2u, 0x10002u})
        {
            std::array<uint8_t, 0x2c> request{};
            std::memcpy(request.data() + 0x28, &format, sizeof(format));
            const auto result = this->call(15, request);
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            const auto& payload = *result.payload;
            std::u16string expected{};
            if (format == 2)
            {
                expected = registry_utils::get_account_domain(this->emu->registry);
                expected.push_back(u'\\');
            }
            expected += registry_utils::get_user_name(this->emu->registry);
            EXPECT_EQ(expected.size() * sizeof(char16_t), read<uint16_t>(payload, 24));
            EXPECT_EQ(expected.size() + 1, read<uint64_t>(payload, 40));
            EXPECT_EQ(expected.size(), read<uint64_t>(payload, 56));
            const auto length = expected.size() * sizeof(char16_t);
            ASSERT_GE(payload.size(), 64 + length);
            EXPECT_EQ(0, std::memcmp(payload.data() + 64, expected.data(), length));
            const auto error_offset = (64 + length + 3) & ~size_t{3};
            EXPECT_EQ(0u, read<uint32_t>(payload, error_offset));
            EXPECT_EQ(STATUS_SUCCESS, read<NTSTATUS>(payload, error_offset + 4));
            EXPECT_EQ(error_offset + 8, payload.size());
        }
    }

    TEST_F(RpcAccountTest, UnsupportedNameFormatReturnsAWellFormedError)
    {
        this->bind(u"\\RPC Control\\lsasspirpc", sspi_interface);
        std::array<uint8_t, 0x2c> request{};
        const uint32_t format = 8;
        std::memcpy(request.data() + 0x28, &format, sizeof(format));
        const auto result = this->call(15, request);
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        EXPECT_EQ(0u, read<uint64_t>(*result.payload, 32));
        EXPECT_EQ(STATUS_NOT_SUPPORTED, read<NTSTATUS>(*result.payload, 44));
    }

    TEST_F(RpcAccountTest, UnmappedSidRepliesRespectNdrPointerWidth)
    {
        for (const bool wow64 : {false, true})
        {
            this->emu->process.is_wow64_process = wow64;
            this->bind(u"\\RPC Control\\lsapolicylookup", lsa_lookup_interface);
            const auto result = this->call(2, {});
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            const auto expected_size = wow64 ? 44u : 56u;
            EXPECT_EQ(expected_size, result.payload->size());
            EXPECT_EQ(0xC0000073u, read<NTSTATUS>(*result.payload, expected_size - 4));
        }
    }

    TEST_F(RpcAccountTest, PolicyQueriesReturnGuestAccountDomainAndExplicitUnsupportedStatus)
    {
        for (const bool classic : {true, false})
        {
            this->bind(classic ? u"\\RPC Control\\LSARPC_ENDPOINT" : u"\\RPC Control\\lsapolicylookup",
                       classic ? lsarpc_interface : lsa_lookup_interface);
            for (const uint16_t information_class : {uint16_t{3}, uint16_t{5}, uint16_t{12}})
            {
                std::array<uint8_t, 22> request{};
                std::memcpy(request.data() + 20, &information_class, sizeof(information_class));
                const auto result = this->call(classic ? 46u : 5u, request);
                ASSERT_EQ(STATUS_SUCCESS, result.status);
                ASSERT_TRUE(result.payload.has_value());
                if (information_class == 3 || information_class == 5)
                {
                    EXPECT_NE(0u, read<uint64_t>(*result.payload, 24));
                    EXPECT_EQ(information_class, read<uint16_t>(*result.payload, 32));
                    const auto domain =
                        information_class == 3 ? std::u16string{u"WORKGROUP"} : registry_utils::get_account_domain(this->emu->registry);
                    EXPECT_EQ(domain.size() * sizeof(char16_t), read<uint16_t>(*result.payload, 40));
                    EXPECT_EQ(information_class == 5, read<uint64_t>(*result.payload, 56) != 0);
                }
                else
                {
                    EXPECT_EQ(0u, read<uint64_t>(*result.payload, 24));
                    EXPECT_EQ(STATUS_INVALID_INFO_CLASS, read<NTSTATUS>(*result.payload, 32));
                }
            }
        }
    }
}
