#include "std_include.hpp"

#include "rpc_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> test_interface{0x5e, 0x6e, 0x5e, 0x6e, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        constexpr uint32_t defer_procedure = 1;
        constexpr uint32_t complete_procedure = 2;
        constexpr uint8_t completion_marker = 0xAB;
        constexpr size_t rpc_response_header_size = 0x18;

        // Defers every call to procedure 1; a call to procedure 2 completes all of them.
        class deferring_port final : public rpc_port
        {
          public:
            NTSTATUS handle_rpc(windows_emulator&, const uint32_t procedure, const lpc_request_context&,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>&) override
            {
                if (procedure == defer_procedure)
                {
                    const auto token = this->defer_current_call();
                    if (!token)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }
                    this->pending.push_back(*token);
                    return STATUS_SUCCESS;
                }

                if (procedure == complete_procedure)
                {
                    for (const auto token : std::exchange(this->pending, {}))
                    {
                        this->complete_deferred_call(token, {completion_marker, 0, 0, 0});
                    }
                    writer.write<uint32_t>(0);
                    return STATUS_SUCCESS;
                }

                return STATUS_NOT_SUPPORTED;
            }

            void complete_outside_a_request(const uint64_t token)
            {
                this->complete_deferred_call(token, {});
            }

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                rpc_port::serialize_object(buffer);
                buffer.write_vector(this->pending);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read_vector(this->pending);
            }

            std::vector<uint64_t> pending{};
        };

        class DeferredRpcTest : public RpcProtocolTest
        {
          protected:
            void SetUp() override
            {
                RpcProtocolTest::SetUp();
                this->bind(std::make_unique<deferring_port>(), test_interface, 8);
            }

            deferring_port& service() const
            {
                return static_cast<deferring_port&>(*this->endpoint);
            }

            std::optional<uint64_t> defer()
            {
                const auto result = this->send_message(defer_procedure, {}, false);
                EXPECT_EQ(STATUS_SUCCESS, result.status);
                EXPECT_TRUE(result.payload.empty());
                return result.deferred_token;
            }

            void complete()
            {
                EXPECT_EQ(STATUS_SUCCESS, this->send_message(complete_procedure, {}).status);
            }
        };
    }

    TEST_F(DeferredRpcTest, DeferredCallGetsNoReplyUntilItIsCompleted)
    {
        ASSERT_TRUE(this->defer().has_value());
        EXPECT_TRUE(this->endpoint->take_completed_replies().empty());
    }

    TEST_F(DeferredRpcTest, CompletionDeliversOneReplyToTheDeferredCall)
    {
        const auto token = this->defer();
        ASSERT_TRUE(token.has_value());
        this->complete();

        const auto replies = this->endpoint->take_completed_replies();
        ASSERT_EQ(1u, replies.size());
        EXPECT_EQ(token, replies[0].deferred_token);
        ASSERT_GT(replies[0].payload.size(), rpc_response_header_size);
        EXPECT_EQ(completion_marker, replies[0].payload[rpc_response_header_size]);
        EXPECT_TRUE(this->endpoint->take_completed_replies().empty());
    }

    TEST_F(DeferredRpcTest, ACallerWaitingForItsReplyIsNotDeferred)
    {
        const auto result = this->send_message(defer_procedure, {});
        EXPECT_FALSE(result.deferred_token.has_value());
        EXPECT_TRUE(this->service().pending.empty());
    }

    // Without a completion port a later reply could not be delivered, so the service never accepts the call as pending, and
    // the same call succeeds once the port is associated.
    TEST_F(DeferredRpcTest, ASendWithoutACompletionPortIsNotDeferredAndCanBeRetried)
    {
        const auto rejected = this->send_message(defer_procedure, {}, false, false);
        EXPECT_FALSE(rejected.deferred_token.has_value());
        EXPECT_TRUE(this->service().pending.empty());

        ASSERT_TRUE(this->defer().has_value());
        EXPECT_EQ(1u, this->service().pending.size());
    }

    TEST_F(DeferredRpcTest, AForgottenRequestIsNotAnswered)
    {
        const auto token = this->defer();
        ASSERT_TRUE(token.has_value());
        this->endpoint->forget_deferred_request(*token);
        this->complete();
        EXPECT_TRUE(this->endpoint->take_completed_replies().empty());
    }

    TEST_F(DeferredRpcTest, CompletionOutsideRequestHandlingIsRejected)
    {
        const auto token = this->defer();
        ASSERT_TRUE(token.has_value());
        EXPECT_THROW(this->service().complete_outside_a_request(*token), std::logic_error);
    }

    TEST_F(DeferredRpcTest, ADeferredRequestSurvivesASnapshot)
    {
        const auto token = this->defer();
        ASSERT_TRUE(token.has_value());

        utils::buffer_serializer serializer{};
        this->endpoint->serialize(serializer);
        auto restored = std::make_unique<deferring_port>();
        utils::buffer_deserializer deserializer{serializer};
        restored->deserialize(deserializer);
        this->endpoint = std::move(restored);

        this->complete();
        const auto replies = this->endpoint->take_completed_replies();
        ASSERT_EQ(1u, replies.size());
        EXPECT_EQ(token, replies[0].deferred_token);
    }
}
