#include "rpc_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> exporter_interface{0xe6, 0x73, 0x0c, 0xe6, 0xf9, 0x88, 0xcf, 0x11,
                                                             0x9a, 0xf1, 0x00, 0x20, 0xaf, 0x6e, 0x72, 0xf4};
        constexpr std::array<uint8_t, 16> endpoint_mapper_interface{0x08, 0x83, 0xaf, 0xe1, 0x1f, 0x5d, 0xc9, 0x11,
                                                                    0x91, 0xa4, 0x08, 0x00, 0x2b, 0x14, 0xa0, 0xfa};
        using connection_context = std::array<uint8_t, 20>;

        class LocalObjectExporterTest : public RpcProtocolTest
        {
          protected:
            std::vector<uint8_t> connect_request(uint32_t count = 3)
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write_ndr_pointer(false);
                writer.write_ndr_pointer(false);
                writer.write_ndr_pointer(false);
                writer.write<uint32_t>(0);
                writer.write<uint16_t>(9);
                writer.write(count);
                return request;
            }

            std::vector<uint8_t> allocate_request(const connection_context& context, uint32_t count = 2)
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(context);
                writer.write(count);
                writer.write<uint32_t>(8);
                writer.write(static_cast<uint32_t>(this->emu->process.process_id));
                writer.write<uint32_t>(5);
                writer.write<uint16_t>(5);
                writer.write<uint16_t>(7);
                writer.write<uint32_t>(3);
                writer.write<uint64_t>(0);
                writer.write_ndr_pointer(false);
                writer.write(std::array<uint8_t, 16>{});
                writer.write<uint32_t>(0x04000000);
                writer.write_ndr_pointer(false);
                writer.write(std::array<uint8_t, 16>{});
                writer.write<uint64_t>(0);
                writer.write<uint32_t>(0);
                return request;
            }
        };
    }

    TEST_F(LocalObjectExporterTest, AllocatesDistinctIdsAndDisconnectsBothNdrWidths)
    {
        for (const size_t width : {size_t{4}, size_t{8}})
        {
            this->bind(u"\\RPC Control\\epmapper", exporter_interface, width);
            auto result = this->call(0, this->connect_request());
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            const auto context = read<connection_context>(*result.payload, 24);
            EXPECT_NE(connection_context{}, context);
            EXPECT_EQ(360u, read<uint32_t>(*result.payload, 44));
            EXPECT_EQ(0u, read<uint32_t>(*result.payload, result.payload->size() - 4));
            result = this->call(6, this->allocate_request(context));
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            const auto oxid = read<std::array<uint8_t, 16>>(*result.payload, 24);
            EXPECT_NE((std::array<uint8_t, 16>{}), oxid);
            const auto first_oid = width == 8 ? 64u : 56u;
            EXPECT_EQ(5u, read<uint64_t>(*result.payload, first_oid));
            EXPECT_EQ(6u, read<uint64_t>(*result.payload, first_oid + 8));
            std::vector<uint8_t> disconnect;
            utils::aligned_binary_writer writer(disconnect, width);
            writer.write(context);
            writer.write<uint32_t>(0);
            result = this->call(10, disconnect);
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            EXPECT_EQ(connection_context{}, read<connection_context>(*result.payload, 24));
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(6, this->allocate_request(context)).status);
        }
    }

    TEST_F(LocalObjectExporterTest, EndpointMapperReportsNoRegisteredEndpoints)
    {
        for (const size_t width : {size_t{4}, size_t{8}})
        {
            this->bind(u"\\RPC Control\\epmapper", endpoint_mapper_interface, width);
            for (const uint32_t procedure : {3u, 7u, 8u})
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, width);
                writer.write_ndr_pointer(false);
                writer.write_ndr_pointer(false);
                writer.write(connection_context{});
                writer.write<uint32_t>(4);
                const auto result = this->call(procedure, request);
                ASSERT_EQ(STATUS_SUCCESS, result.status);
                ASSERT_TRUE(result.payload.has_value());
                EXPECT_EQ(connection_context{}, read<connection_context>(*result.payload, 24));
                EXPECT_EQ(0u, read<uint32_t>(*result.payload, 44));
                EXPECT_EQ(4u, read<uint32_t>(*result.payload, 48));
                EXPECT_EQ(0x16c9a0d6u, read<uint32_t>(*result.payload, result.payload->size() - 4));
            }
            EXPECT_EQ(STATUS_NOT_SUPPORTED, this->call(2, {}).status);
        }
    }

    TEST_F(LocalObjectExporterTest, RegistrationSurvivesSerializationAndCanBeReleased)
    {
        this->bind(u"\\RPC Control\\epmapper", exporter_interface, 4);
        auto result = this->call(0, this->connect_request());
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        const auto context = read<connection_context>(*result.payload, 24);
        result = this->call(6, this->allocate_request(context));
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        const auto oxid = read<std::array<uint8_t, 16>>(*result.payload, 24);
        utils::buffer_serializer state;
        this->endpoint->serialize(state);
        utils::buffer_deserializer reader{state};
        this->endpoint->deserialize(reader);
        std::vector<uint8_t> release;
        utils::aligned_binary_writer writer(release, this->pointer_size);
        writer.write(context);
        writer.write(oxid);
        writer.write<uint32_t>(0);
        writer.write_pointer_sized(0);
        EXPECT_EQ(STATUS_SUCCESS, this->call(8, release).status);
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->call(8, release).status);
    }

    TEST_F(LocalObjectExporterTest, RejectsTruncatedRequestsAndExcessiveReservations)
    {
        this->bind(u"\\RPC Control\\epmapper", exporter_interface, 4);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(0, {}).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(0, this->connect_request(0x20001)).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(6, {}).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(10, {}).status);
    }
}
