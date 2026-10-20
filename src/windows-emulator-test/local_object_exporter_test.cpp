#include "rpc_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> exporter_interface{0xe6, 0x73, 0x0c, 0xe6, 0xf9, 0x88, 0xcf, 0x11,
                                                             0x9a, 0xf1, 0x00, 0x20, 0xaf, 0x6e, 0x72, 0xf4};
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

            std::vector<uint8_t> free_request(const connection_context& context, uint64_t oxid, const std::vector<uint64_t>& object_ids)
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(context);
                writer.write(oxid);
                writer.write(static_cast<uint32_t>(object_ids.size()));
                writer.write(static_cast<uint32_t>(object_ids.size()));
                for (const auto id : object_ids)
                {
                    writer.write(id);
                }
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
            const auto server_oxid = read<uint64_t>(*result.payload, 24);
            const auto primary_oxid = read<uint64_t>(*result.payload, 32);
            EXPECT_NE(0u, server_oxid);
            EXPECT_NE(0u, primary_oxid);
            EXPECT_NE(server_oxid, primary_oxid);
            const auto first_oid = width == 8 ? 64u : 56u;
            EXPECT_EQ(5u, read<uint64_t>(*result.payload, first_oid));
            EXPECT_EQ(6u, read<uint64_t>(*result.payload, first_oid + 8));
            const auto second_result = this->call(6, this->allocate_request(context, 1));
            ASSERT_EQ(STATUS_SUCCESS, second_result.status);
            ASSERT_TRUE(second_result.payload.has_value());
            EXPECT_NE(server_oxid, read<uint64_t>(*second_result.payload, 24));
            EXPECT_EQ(primary_oxid, read<uint64_t>(*second_result.payload, 32));
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
        const auto server_oxid = read<uint64_t>(*result.payload, 24);
        const auto primary_oxid = read<uint64_t>(*result.payload, 32);
        utils::buffer_serializer state;
        this->endpoint->serialize(state);
        utils::buffer_deserializer reader{state};
        this->endpoint->deserialize(reader);
        EXPECT_EQ(STATUS_SUCCESS, this->call(8, this->free_request(context, primary_oxid, {})).status);
        EXPECT_EQ(STATUS_SUCCESS, this->call(8, this->free_request(context, server_oxid, {5, 6})).status);
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->call(8, this->free_request(context, primary_oxid, {})).status);
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
