#include "std_include.hpp"

#include "rpc_test_utils.hpp"
#include <io_completion_wait.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> test_interface{0x5e, 0x6e, 0x5e, 0x6e, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        constexpr uint32_t defer_procedure = 1;
        constexpr uint32_t complete_procedure = 2;
        constexpr uint32_t complete_large_procedure = 3;
        constexpr size_t large_reply_size = 0x8000;
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

                if (procedure == complete_procedure || procedure == complete_large_procedure)
                {
                    std::vector<uint8_t> reply{completion_marker, 0, 0, 0};
                    if (procedure == complete_large_procedure)
                    {
                        reply.resize(large_reply_size);
                        for (size_t i = 0; i < reply.size(); ++i)
                        {
                            reply[i] = static_cast<uint8_t>(i * 7);
                        }
                    }
                    for (const auto token : std::exchange(this->pending, {}))
                    {
                        this->complete_deferred_call(token, reply);
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

    // A reply too big for an inline message travels in a view, as an immediate one does: the message keeps the larger RPC
    // header with the large-payload flag and the NDR size at 0x18.
    TEST_F(DeferredRpcTest, ALargeDeferredReplyIsSentInAView)
    {
        const auto token = this->defer();
        ASSERT_TRUE(token.has_value());
        EXPECT_EQ(STATUS_SUCCESS, this->send_message(complete_large_procedure, {}).status);

        const auto replies = this->endpoint->take_completed_replies();
        ASSERT_EQ(1u, replies.size());
        ASSERT_EQ(0x20u, replies[0].payload.size());
        EXPECT_EQ(4u, read<uint32_t>(replies[0].payload, 0x08));
        EXPECT_EQ(large_reply_size, read<uint32_t>(replies[0].payload, 0x18));
        EXPECT_EQ(static_cast<uint32_t>(*token), read<uint32_t>(replies[0].payload, 12));
        ASSERT_EQ(large_reply_size, replies[0].view_payload.size());
        for (const size_t index : {size_t{0}, size_t{1}, large_reply_size - 1})
        {
            EXPECT_EQ(static_cast<uint8_t>(index * 7), replies[0].view_payload[index]) << index;
        }
        EXPECT_NE(0, replies[0].message.native.u2.s2.Type & lpc_continuation_required);
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

    namespace
    {
        constexpr uint64_t completion_key = 0x1234;

        // The deferring service behind a port container, which delivers deferred replies through its completion port.
        class DeferredRpcContainerTest : public RpcProtocolTest
        {
          protected:
            void SetUp() override
            {
                RpcProtocolTest::SetUp();
                this->port_handle = this->emu->process.ports.store(port_container{u"DeferringService", std::make_unique<deferring_port>()});
                this->completion = this->emu->process.io_completions.store(io_completion{});

                std::array<uint8_t, 72> bind{};
                bind[0] = 1;
                std::memcpy(bind.data() + 12, test_interface.data(), test_interface.size());
                bind[32] = 3;
                ASSERT_EQ(STATUS_SUCCESS, this->send(bind, 0, true).status);
            }

            port_container& container()
            {
                return *this->emu->process.ports.get(this->port_handle);
            }

            deferring_port& service()
            {
                return *this->container().get_internal_port<deferring_port>();
            }

            NTSTATUS associate()
            {
                return this->container().associate_completion_port(*this->emu, this->emu->vcpu(0).active_thread, completion_key,
                                                                   this->completion);
            }

            lpc_message_result send(const std::span<const uint8_t> body, const uint64_t message_context, const bool wait_for_reply)
            {
                PORT_MESSAGE64 header{};
                header.u1.s1.DataLength = static_cast<CSHORT>(body.size());
                header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(header) + body.size());
                header.u2.s2.Type = LPC_REQUEST;
                this->emu->memory.write_memory(this->storage, &header, sizeof(header));
                this->emu->memory.write_memory(this->storage + sizeof(header), body.data(), body.size());

                lpc_message_context context{this->emu->memory};
                context.send_message = {this->emu->memory, this->storage};
                context.send_message_context = message_context;
                if (wait_for_reply)
                {
                    context.receive_message = {this->emu->memory, this->storage + 0x4000};
                    context.receive_buffer_length = 0x1000;
                }
                return this->container().handle_message(*this->emu, context);
            }

            lpc_message_result call(const uint32_t procedure, const uint64_t message_context, const bool wait_for_reply)
            {
                std::array<uint8_t, 64> body{};
                const auto call_id = ++this->last_call_id;
                std::memcpy(body.data() + 12, &call_id, sizeof(call_id));
                std::memcpy(body.data() + 20, &procedure, sizeof(procedure));
                return this->send(body, message_context, wait_for_reply);
            }

            lpc_message_result receive()
            {
                lpc_message_context context{this->emu->memory};
                context.receive_message = {this->emu->memory, this->storage + 0x4000};
                context.receive_buffer_length = 0x1000;
                return this->container().handle_message(*this->emu, context);
            }

            bool dequeue()
            {
                io_completion_message message{};
                if (!io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message))
                {
                    return false;
                }
                EXPECT_EQ(completion_key, message.key_context);
                return true;
            }

            handle port_handle{};
            handle completion{};
        };
    }

    TEST_F(DeferredRpcContainerTest, EachDeferredCallGetsOneCompletionAndItsReply)
    {
        // Sent without a completion port, the call is not taken on; once the port is associated the same call is.
        (void)this->call(defer_procedure, 0x10, false);
        EXPECT_TRUE(this->service().pending.empty());
        EXPECT_FALSE(this->container().has_pending_reply());
        ASSERT_EQ(STATUS_SUCCESS, this->associate());

        ASSERT_EQ(STATUS_SUCCESS, this->call(defer_procedure, 0x11, false).status);
        ASSERT_EQ(STATUS_SUCCESS, this->call(defer_procedure, 0x22, false).status);
        EXPECT_EQ(2u, this->service().pending.size());
        EXPECT_FALSE(this->dequeue());
        EXPECT_FALSE(this->container().has_pending_reply());

        ASSERT_EQ(STATUS_SUCCESS, this->call(complete_procedure, 0x33, true).status);

        // Each reply carries the message context and the RPC call id of the call it answers.
        std::set<std::pair<uint64_t, uint32_t>> answered{};
        for (int i = 0; i < 2; ++i)
        {
            ASSERT_TRUE(this->dequeue());
            const auto reply = this->receive();
            ASSERT_EQ(STATUS_SUCCESS, reply.status);
            ASSERT_GT(reply.payload.size(), rpc_response_header_size);
            EXPECT_EQ(completion_marker, reply.payload[rpc_response_header_size]);
            answered.emplace(reply.message_context, read<uint32_t>(reply.payload, 12));
        }
        EXPECT_EQ((std::set<std::pair<uint64_t, uint32_t>>{{0x11, 2}, {0x22, 3}}), answered);
        EXPECT_FALSE(this->dequeue());
        EXPECT_FALSE(this->container().has_pending_reply());
    }
}
