#include "std_include.hpp"

#include "rpc_test_utils.hpp"
#include <ports/nsi_service.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t register_change_notification = 5;
        constexpr uint32_t deregister_change_notification = 6;
        constexpr uint32_t request_change_notification = 7;
        constexpr uint32_t error_cancelled = 1223;
        constexpr size_t rpc_response_header_size = 0x18;
        constexpr size_t context_handle_size = 20;

        using context_handle = std::array<uint8_t, context_handle_size>;

        class NsiServiceTest : public RpcProtocolTest
        {
          protected:
            void bind_service(const size_t width)
            {
                this->bind(u"\\RPC Control\\" + std::u16string(nsi_service_endpoint), nsi_service_interface, width);
            }

            // The registration descriptor as winnsi marshals it: a ULONG, a unique pointer to the NPI_MODULEID and a ULONG (NDR64
            // keeps the 24-byte memory layout, classic NDR packs 12 bytes), then the module id: Length, Type and the union
            // with its discriminant, here a GUID.
            void write_registration_descriptor(utils::aligned_binary_writer& writer, const uint32_t discriminant) const
            {
                writer.write<uint32_t>(7);
                writer.write_ndr_pointer(true);
                writer.write<uint32_t>(1);
                writer.align_to(this->pointer_size == 8 ? 8 : 4);
                writer.write<uint16_t>(0x18);
                // Type and the union discriminant are enum16: two bytes in classic NDR, four in NDR64.
                if (this->pointer_size == 8)
                {
                    writer.write<uint32_t>(1);
                    writer.write<uint32_t>(discriminant);
                }
                else
                {
                    writer.write<uint16_t>(1);
                    writer.write<uint16_t>(static_cast<uint16_t>(discriminant));
                }
                const std::array<uint8_t, 16> guid{0xeb, 0x00, 0x4a, 0xf2, 0x51, 0x86, 0xd1, 0x11,
                                                   0x99, 0x89, 0x00, 0xa0, 0xc9, 0x0c, 0x0c, 0xd2};
                writer.write(guid.data(), guid.size(), 4);
            }

            std::vector<uint8_t> registration_descriptor(const uint32_t discriminant = 1) const
            {
                std::vector<uint8_t> data{};
                utils::aligned_binary_writer writer(data, this->pointer_size);
                this->write_registration_descriptor(writer, discriminant);
                return data;
            }

            // The descriptor, then the 8-byte structure the call passes by value.
            std::vector<uint8_t> registration_request(const uint32_t discriminant = 1) const
            {
                std::vector<uint8_t> data{};
                utils::aligned_binary_writer writer(data, this->pointer_size);
                this->write_registration_descriptor(writer, discriminant);
                writer.align_to(4);
                writer.write<uint32_t>(0);
                writer.write<uint32_t>(0);
                return data;
            }

            context_handle register_notification()
            {
                const auto reply = this->call(register_change_notification, this->registration_request());
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                context_handle context{};
                if (!reply.payload || reply.payload->size() < rpc_response_header_size + context.size())
                {
                    ADD_FAILURE() << "Registration returned no context handle";
                    return context;
                }
                std::memcpy(context.data(), reply.payload->data() + rpc_response_header_size, context.size());
                return context;
            }

            lpc_message_result request_change(const context_handle& context)
            {
                return this->send_message(request_change_notification, context, false);
            }

            lpc_message_result deregister(const context_handle& context)
            {
                auto request = this->registration_descriptor();
                request.insert(request.end(), context.begin(), context.end());
                return this->send_message(deregister_change_notification, request);
            }

            std::vector<lpc_message_result> completed_replies() const
            {
                return this->endpoint->take_completed_replies();
            }
        };
    }

    TEST_F(NsiServiceTest, RegistrationDecodesTheDescriptorInBothNdrEncodings)
    {
        for (const size_t width : {size_t{4}, size_t{8}})
        {
            this->bind_service(width);
            const auto context = this->register_notification();
            EXPECT_EQ(STATUS_SUCCESS, this->deregister(context).status) << width;
        }
    }

    // A 32-bit winnsi sends the 12-byte descriptor, Length at 12, Type at 14, the discriminant at 16 and the GUID at 20, then
    // the by-value structure: 44 bytes.
    TEST_F(NsiServiceTest, ClassicRegistrationHasWinnsisLayout)
    {
        this->bind_service(4);
        const auto request = this->registration_request();
        ASSERT_EQ(44u, request.size());
        EXPECT_EQ(1u, read<uint16_t>(request, 14));
        EXPECT_EQ(1u, read<uint16_t>(request, 16));
        EXPECT_EQ(0xf24a00ebu, read<uint32_t>(request, 20));
        EXPECT_EQ(STATUS_SUCCESS, this->call(register_change_notification, request).status);
    }

    TEST_F(NsiServiceTest, MalformedRegistrationsAreRejected)
    {
        this->bind_service(8);
        EXPECT_NE(STATUS_SUCCESS, this->call(register_change_notification, {}).status);
        EXPECT_NE(STATUS_SUCCESS, this->call(register_change_notification, std::vector<uint8_t>{0}).status);
        // A union discriminant that disagrees with the module id's type, and one that names no arm.
        EXPECT_NE(STATUS_SUCCESS, this->call(register_change_notification, this->registration_request(2)).status);
        EXPECT_NE(STATUS_SUCCESS, this->call(register_change_notification, this->registration_request(9)).status);

        auto truncated = this->registration_request();
        truncated.pop_back();
        EXPECT_NE(STATUS_SUCCESS, this->call(register_change_notification, truncated).status);

        auto trailing = this->registration_request();
        trailing.push_back(0);
        EXPECT_NE(STATUS_SUCCESS, this->call(register_change_notification, trailing).status);
    }

    TEST_F(NsiServiceTest, ChangeRequestStaysPendingUntilDeregistrationCancelsIt)
    {
        this->bind_service(8);
        const auto context = this->register_notification();

        const auto pending = this->request_change(context);
        ASSERT_EQ(STATUS_SUCCESS, pending.status);
        ASSERT_TRUE(pending.deferred_token.has_value());
        EXPECT_TRUE(this->completed_replies().empty());

        ASSERT_EQ(STATUS_SUCCESS, this->deregister(context).status);
        const auto replies = this->completed_replies();
        ASSERT_EQ(1u, replies.size());
        EXPECT_EQ(pending.deferred_token, replies[0].deferred_token);
        ASSERT_GE(replies[0].payload.size(), sizeof(uint32_t));
        EXPECT_EQ(error_cancelled, read<uint32_t>(replies[0].payload, replies[0].payload.size() - sizeof(uint32_t)));
    }

    TEST_F(NsiServiceTest, CancellationUsesTheEncodingOfTheRequestItAnswersAfterARestore)
    {
        this->bind_service(4);
        const auto context = this->register_notification();
        ASSERT_EQ(STATUS_SUCCESS, this->request_change(context).status);

        utils::buffer_serializer serializer{};
        this->endpoint->serialize(serializer);
        auto restored = create_port(u"\\RPC Control\\" + std::u16string(nsi_service_endpoint));
        utils::buffer_deserializer deserializer{serializer};
        restored->deserialize(deserializer);
        this->endpoint = std::move(restored);

        ASSERT_EQ(STATUS_SUCCESS, this->deregister(context).status);
        const auto replies = this->completed_replies();
        ASSERT_EQ(1u, replies.size());
        ASSERT_GE(replies[0].payload.size(), sizeof(uint32_t));
        // Classic NDR: two null pointers, a key size, a two-byte enum padded to four, a parameter size, an offset, the
        // notification enum and the status.
        constexpr size_t classic_ndr_cancellation_size = 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4;
        EXPECT_EQ(rpc_response_header_size + classic_ndr_cancellation_size, replies[0].payload.size());
        EXPECT_EQ(error_cancelled, read<uint32_t>(replies[0].payload, replies[0].payload.size() - sizeof(uint32_t)));
    }

    TEST_F(NsiServiceTest, ContextHandleOnlyCountsAtItsParameterPosition)
    {
        this->bind_service(8);
        const auto context = this->register_notification();

        // The registration's handle inside the descriptor, followed by bytes that are no context handle.
        std::vector<uint8_t> request(context.begin(), context.end());
        request.resize(request.size() + context_handle_size + 4, 0x22);
        EXPECT_NE(STATUS_SUCCESS, this->send_message(deregister_change_notification, request).status);

        std::vector<uint8_t> shifted(4, 0);
        shifted.insert(shifted.end(), context.begin(), context.end());
        EXPECT_NE(STATUS_SUCCESS, this->send_message(request_change_notification, shifted, false).status);

        EXPECT_EQ(STATUS_SUCCESS, this->deregister(context).status);
    }

    TEST_F(NsiServiceTest, ADeregisteredContextIsNoLongerValid)
    {
        this->bind_service(8);
        const auto context = this->register_notification();
        ASSERT_EQ(STATUS_SUCCESS, this->deregister(context).status);

        EXPECT_NE(STATUS_SUCCESS, this->deregister(context).status);
        EXPECT_NE(STATUS_SUCCESS, this->request_change(context).status);
        EXPECT_TRUE(this->completed_replies().empty());
    }
}
