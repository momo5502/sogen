#include "rpc_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> event_log_interface{0xdc, 0x3f, 0x27, 0x82, 0x2a, 0xe3, 0xc3, 0x18,
                                                              0x3f, 0x78, 0x82, 0x79, 0x29, 0xdc, 0x23, 0xea};
        using event_context = std::array<uint8_t, 20>;

        class EventLogTest : public RpcProtocolTest
        {
          protected:
            std::vector<uint8_t> open_request(uint32_t version = 1, std::u16string_view server = {})
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write_ndr_pointer(!server.empty());
                if (!server.empty())
                {
                    writer.write_ndr_u16string(server);
                }
                write_unicode_string(writer, u"PowerShell");
                write_unicode_string(writer, u"Application");
                writer.write(version);
                writer.write<uint32_t>(1);
                return request;
            }

            std::vector<uint8_t> report_request(const event_context& context, bool extended = false, uint16_t count = 1)
            {
                std::vector<uint8_t> request;
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(context);
                writer.write<uint32_t>(123);
                if (extended)
                {
                    writer.write<uint32_t>(1);
                }
                writer.write<uint16_t>(4);
                writer.write<uint16_t>(0);
                writer.write<uint32_t>(400);
                writer.write(count);
                writer.write<uint32_t>(3);
                write_unicode_string(writer, u"guest-computer");
                writer.write_ndr_pointer(false);
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(1);
                writer.write_ndr_pointer(true);
                write_unicode_string(writer, u"powershell-event-ok");
                writer.write_ndr_pointer(true);
                writer.write_pointer_sized(3);
                writer.write(std::array<uint8_t, 3>{0xde, 0xad, 0xfe});
                writer.write<uint16_t>(0);
                writer.write_ndr_pointer(true);
                writer.write<uint32_t>(0);
                if (!extended)
                {
                    writer.write_ndr_pointer(true);
                    writer.write<uint32_t>(0);
                }
                return request;
            }
        };
    }

    TEST_F(EventLogTest, RegistersReportsAndClosesBothNdrWidths)
    {
        for (const size_t width : {size_t{4}, size_t{8}})
        {
            this->bind(u"\\RPC Control\\eventlog", event_log_interface, width);
            auto result = this->call(8, this->open_request());
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            const auto context = read<event_context>(*result.payload, 24);
            EXPECT_NE(event_context{}, context);
            EXPECT_EQ(STATUS_SUCCESS, read<NTSTATUS>(*result.payload, 44));
            for (const bool extended : {false, true})
            {
                result = this->call(extended ? 25u : 11u, this->report_request(context, extended));
                ASSERT_EQ(STATUS_SUCCESS, result.status);
                ASSERT_TRUE(result.payload.has_value());
                EXPECT_EQ(STATUS_SUCCESS, read<NTSTATUS>(*result.payload, result.payload->size() - 4));
            }
            result = this->call(4, context);
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            EXPECT_EQ(2u, read<uint32_t>(*result.payload, 24));
            EXPECT_EQ(STATUS_SUCCESS, read<NTSTATUS>(*result.payload, 28));
            result = this->call(3, context);
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            EXPECT_EQ(event_context{}, read<event_context>(*result.payload, 24));
            result = this->call(4, context);
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            ASSERT_TRUE(result.payload.has_value());
            EXPECT_EQ(STATUS_INVALID_HANDLE, read<NTSTATUS>(*result.payload, 28));
        }
    }

    TEST_F(EventLogTest, RecordsAndOpenContextsSurviveSerialization)
    {
        this->bind(u"\\RPC Control\\eventlog", event_log_interface, 4);
        auto result = this->call(8, this->open_request());
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        const auto context = read<event_context>(*result.payload, 24);
        ASSERT_EQ(STATUS_SUCCESS, this->call(11, this->report_request(context)).status);
        utils::buffer_serializer state;
        this->endpoint->serialize(state);
        utils::buffer_serializer expected_message;
        expected_message.write(std::u16string{u"powershell-event-ok"});
        const auto& message_bytes = expected_message.get_buffer();
        const auto& snapshot = state.get_buffer();
        EXPECT_NE(snapshot.end(), std::search(snapshot.begin(), snapshot.end(), message_bytes.begin(), message_bytes.end()));
        utils::buffer_serializer expected_data;
        expected_data.write_vector(std::vector<uint8_t>{0xde, 0xad, 0xfe});
        const auto& event_data = expected_data.get_buffer();
        EXPECT_NE(snapshot.end(), std::search(snapshot.begin(), snapshot.end(), event_data.begin(), event_data.end()));
        utils::buffer_deserializer reader{state};
        this->endpoint->deserialize(reader);
        result = this->call(4, context);
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        EXPECT_EQ(1u, read<uint32_t>(*result.payload, 24));
    }

    TEST_F(EventLogTest, RejectsMalformedEventsWithoutStoringRecords)
    {
        this->bind(u"\\RPC Control\\eventlog", event_log_interface, 4);
        auto result = this->call(8, this->open_request());
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        const auto context = read<event_context>(*result.payload, 24);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(11, context).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(11, this->report_request(context, false, 257)).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(8, {}).status);
        result = this->call(4, context);
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        EXPECT_EQ(0u, read<uint32_t>(*result.payload, 24));
        result = this->call(8, this->open_request(2));
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        ASSERT_TRUE(result.payload.has_value());
        EXPECT_EQ(event_context{}, read<event_context>(*result.payload, 24));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, read<NTSTATUS>(*result.payload, 44));
    }

    TEST_F(EventLogTest, SupportsLocalServerNameAndRejectsRemoteRequests)
    {
        this->bind(u"\\RPC Control\\eventlog", event_log_interface, 4);
        EXPECT_EQ(STATUS_SUCCESS, this->call(8, this->open_request(1, u".")).status);
        EXPECT_EQ(STATUS_NOT_SUPPORTED, this->call(8, this->open_request(1, u"remote-server")).status);
    }
}
