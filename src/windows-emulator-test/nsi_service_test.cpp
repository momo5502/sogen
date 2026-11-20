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

            // What to watch: a module id pointer and the object; the service only needs it present.
            static std::vector<uint8_t> registration_descriptor()
            {
                return std::vector<uint8_t>(24, 0x11);
            }

            context_handle register_notification()
            {
                const auto reply = this->call(register_change_notification, registration_descriptor());
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
                auto request = registration_descriptor();
                request.insert(request.end(), context.begin(), context.end());
                return this->send_message(deregister_change_notification, request);
            }

            std::vector<lpc_message_result> completed_replies() const
            {
                return this->endpoint->take_completed_replies();
            }
        };
    }

    TEST_F(NsiServiceTest, RegistrationWithoutADescriptorIsRejected)
    {
        this->bind_service(8);
        EXPECT_NE(STATUS_SUCCESS, this->call(register_change_notification, {}).status);
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
