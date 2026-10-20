#include "rpc_test_utils.hpp"
#include <io_completion_wait.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> endpoint_mapper_interface{0x08, 0x83, 0xaf, 0xe1, 0x1f, 0x5d, 0xc9, 0x11,
                                                                    0x91, 0xa4, 0x08, 0x00, 0x2b, 0x14, 0xa0, 0xfa};
        constexpr uint64_t completion_key = 0x1234;
        constexpr uint64_t message_context = 0xabcd;
        constexpr ULONG handshake_size = 72;

        class AlpcCompletionTest : public RpcProtocolTest
        {
          protected:
            void SetUp() override
            {
                RpcProtocolTest::SetUp();
                this->port_handle = this->emu->process.ports.store(port_container{u"\\RPC Control\\epmapper", *this->emu, {}});
                this->completion = this->emu->process.io_completions.store(io_completion{});
            }

            port_container& container()
            {
                return *this->emu->process.ports.get(this->port_handle);
            }

            NTSTATUS associate()
            {
                return this->container().associate_completion_port(*this->emu, this->emu->vcpu(0).active_thread, completion_key,
                                                                   this->completion);
            }

            lpc_message_result send_handshake()
            {
                this->emu->memory.set_memory(this->storage, 0, 0x1000);
                PORT_MESSAGE64 message{};
                message.u1.s1.DataLength = handshake_size;
                message.u1.s1.TotalLength = sizeof(message) + handshake_size;
                emulator_object<PORT_MESSAGE64>{this->emu->memory, this->storage}.write(message);
                const auto payload = this->storage + sizeof(message);
                this->emu->memory.write_memory(payload, uint32_t{1});
                this->emu->memory.write_memory(payload + 12, endpoint_mapper_interface.data(), endpoint_mapper_interface.size());
                this->emu->memory.write_memory(payload + 32, uint32_t{1});

                lpc_message_context context{this->emu->memory};
                context.send_message = {this->emu->memory, this->storage};
                context.send_message_context = message_context;
                return this->container().handle_message(*this->emu, context);
            }

            lpc_message_result receive()
            {
                lpc_message_context context{this->emu->memory};
                context.receive_message = {this->emu->memory, this->storage + 0x2000};
                context.receive_buffer_length = 0x1000;
                return this->container().handle_message(*this->emu, context);
            }

            bool dequeue(io_completion_message& message)
            {
                return io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message);
            }

            handle port_handle{};
            handle completion{};
        };
    }

    TEST_F(AlpcCompletionTest, SendOnlyMessageWithoutAssociationIsNotQueued)
    {
        EXPECT_EQ(STATUS_SUCCESS, this->send_handshake().status);
        EXPECT_FALSE(this->container().has_pending_reply());
        io_completion_message message{};
        EXPECT_FALSE(this->dequeue(message));
    }

    TEST_F(AlpcCompletionTest, SendOnlyMessageWithAssociationQueuesReplyAndPostsCompletion)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->associate());
        ASSERT_EQ(STATUS_SUCCESS, this->send_handshake().status);

        io_completion_message message{};
        ASSERT_TRUE(this->dequeue(message));
        EXPECT_EQ(completion_key, message.key_context);
        EXPECT_FALSE(this->dequeue(message));

        ASSERT_TRUE(this->container().has_pending_reply());
        const auto reply = this->receive();
        ASSERT_EQ(STATUS_SUCCESS, reply.status);
        EXPECT_EQ(static_cast<USHORT>(LPC_REPLY), reply.message.native.u2.s2.Type);
        EXPECT_EQ(message_context, reply.message_context);
        EXPECT_EQ(handshake_size, reply.payload.size());
        EXPECT_FALSE(this->container().has_pending_reply());
    }

    TEST_F(AlpcCompletionTest, AsyncConnectionReplyIsDeliveredOnceTheCompletionPortIsAssociated)
    {
        this->container().queue_connection_reply(*this->emu, false);
        io_completion_message message{};
        EXPECT_FALSE(this->dequeue(message));

        ASSERT_EQ(STATUS_SUCCESS, this->associate());
        ASSERT_TRUE(this->dequeue(message));
        EXPECT_EQ(completion_key, message.key_context);

        const auto reply = this->receive();
        ASSERT_EQ(STATUS_SUCCESS, reply.status);
        EXPECT_EQ(static_cast<USHORT>(LPC_CONNECTION_REPLY), reply.message.native.u2.s2.Type);
        EXPECT_NE(0u, reply.message.native.MessageId);
        EXPECT_TRUE(reply.payload.empty());
    }

    TEST_F(AlpcCompletionTest, AssociationPostsOneCompletionPerAlreadyQueuedMessage)
    {
        this->container().queue_connection_reply(*this->emu, false);
        this->container().queue_connection_reply(*this->emu, false);
        ASSERT_EQ(STATUS_SUCCESS, this->associate());

        io_completion_message message{};
        EXPECT_TRUE(this->dequeue(message));
        EXPECT_TRUE(this->dequeue(message));
        EXPECT_FALSE(this->dequeue(message));
    }

    TEST_F(AlpcCompletionTest, AssociationKeepsCompletionPortAliveAfterTheOriginalHandleIsClosed)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->associate());
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->associate());

        auto original = this->completion;
        io_completion_wait::release_handle_reference(this->emu->process, original);
        ASSERT_EQ(STATUS_SUCCESS, this->send_handshake().status);

        const auto retained = this->container().completion_port();
        ASSERT_NE(nullptr, this->emu->process.io_completions.get(retained));
        EXPECT_EQ(1u, this->emu->process.io_completions.get(retained)->queue.size());

        this->container().release_completion_port(this->emu->process);
        EXPECT_EQ(nullptr, this->emu->process.io_completions.get(retained));
        EXPECT_EQ(0u, this->container().completion_port().bits);
    }
}
