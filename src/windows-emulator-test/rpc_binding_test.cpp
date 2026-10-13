#include "emulation_test_utils.hpp"
#include <binary_writer.hpp>
#include <port.hpp>

namespace sogen::test
{
    namespace
    {
        struct binding_probe_port final : rpc_port
        {
            std::vector<uint8_t> received_data{};
            uint64_t received_handle{};

            NTSTATUS handle_rpc(windows_emulator& emu, uint32_t procedure, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>&) override
            {
                this->received_data.resize(c.send_buffer_length);
                if (!this->received_data.empty())
                {
                    emu.memory.read_memory(c.send_buffer, this->received_data.data(), this->received_data.size());
                }
                this->received_handle = c.send_handle;
                writer.write(this->bound_interface().data(), this->bound_interface().size());
                writer.write(static_cast<uint32_t>(writer.pointer_size()));
                writer.write(procedure);
                return STATUS_SUCCESS;
            }
        };

        class RpcBindingTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            lpc_request_result request(ULONG size, uint64_t view_base = 0, uint64_t view_size = 0)
            {
                return this->endpoint.handle_request(*this->emu, {.send_buffer = this->storage,
                                                                  .send_buffer_length = size,
                                                                  .recv_buffer = this->storage + 0x800,
                                                                  .recv_buffer_length = 0x800,
                                                                  .send_handle = 0x1234,
                                                                  .send_view_base = view_base,
                                                                  .send_view_size = view_size});
            }

            void bind(uint16_t context, uint16_t syntax, uint8_t interface_tag)
            {
                this->emu->memory.set_memory(this->storage, 0, 0x1000);
                this->emu->memory.write_memory(this->storage, uint32_t{1});
                this->emu->memory.write_memory(this->storage + 12, interface_tag);
                this->emu->memory.write_memory(this->storage + 32, static_cast<uint32_t>(syntax));
                const auto context_offset = syntax == 3 ? 38u : 36u;
                this->emu->memory.write_memory(this->storage + context_offset, context);
                const auto reply = this->request(72);
                ASSERT_EQ(STATUS_SUCCESS, reply.status);
                ASSERT_TRUE(reply.payload.has_value());
                EXPECT_EQ(interface_tag, reply.payload->at(12));
                EXPECT_EQ(syntax == 3 ? 2 : syntax, reply.payload->at(32));
                uint16_t returned_context{};
                std::memcpy(&returned_context, reply.payload->data() + context_offset, sizeof(returned_context));
                EXPECT_EQ(context, returned_context);
            }

            lpc_request_result call(uint16_t context)
            {
                this->emu->memory.set_memory(this->storage, 0, 0x800);
                this->emu->memory.write_memory(this->storage + 12, uint32_t{123});
                this->emu->memory.write_memory(this->storage + 16, context);
                this->emu->memory.write_memory(this->storage + 20, uint32_t{17});
                return this->request(64);
            }

            void expect_call(uint16_t context, uint8_t interface_tag, uint32_t width)
            {
                const auto reply = this->call(context);
                ASSERT_EQ(STATUS_SUCCESS, reply.status);
                ASSERT_TRUE(reply.payload.has_value());
                ASSERT_EQ(48u, reply.payload->size());
                EXPECT_EQ(interface_tag, reply.payload->at(24));
                uint32_t returned_width{};
                std::memcpy(&returned_width, reply.payload->data() + 40, sizeof(returned_width));
                EXPECT_EQ(width, returned_width);
                EXPECT_EQ(17u, reply.payload->at(44));
            }

            std::unique_ptr<windows_emulator> emu{};
            binding_probe_port endpoint{};
            uint64_t storage{};
        };
    }

    TEST_F(RpcBindingTest, RoutesMultipleContextsWithNegotiatedNdrWidths)
    {
        this->bind(0, 1, 0x11);
        this->bind(7, 3, 0x22);
        this->expect_call(0, 0x11, 4);
        this->expect_call(7, 0x22, 8);
        this->expect_call(0, 0x11, 4);
    }

    TEST_F(RpcBindingTest, RejectsUnknownPresentationContext)
    {
        this->bind(0, 1, 0x11);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->call(7).status);
    }

    TEST_F(RpcBindingTest, RejectsUnsupportedTransferSyntaxWithoutReplacingExistingBinding)
    {
        this->bind(0, 1, 0x11);
        for (const auto syntax : {0u, 4u, 0xffffffffu})
        {
            this->emu->memory.write_memory(this->storage + 32, syntax);
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->request(72).status);
        }
        this->expect_call(0, 0x11, 4);
    }

    TEST_F(RpcBindingTest, PreservesAllContextsAcrossSerialization)
    {
        this->bind(0, 1, 0x11);
        this->bind(7, 3, 0x22);
        utils::buffer_serializer snapshot;
        this->endpoint.serialize_object(snapshot);
        utils::buffer_deserializer input{snapshot};
        this->endpoint.deserialize_object(input);
        this->expect_call(0, 0x11, 4);
        this->expect_call(7, 0x22, 8);
    }

    TEST_F(RpcBindingTest, ReadsLargeRequestDataFromViewForBothNdrWidths)
    {
        for (const auto syntax : {uint16_t{1}, uint16_t{3}})
        {
            this->bind(0, syntax, 0x11);
            ASSERT_EQ(STATUS_SUCCESS, this->call(0).status);
            std::vector<uint8_t> data(0x5000);
            uint8_t value{};
            for (auto& byte : data)
            {
                byte = value++;
            }
            const auto view = this->emu->memory.allocate_memory(data.size(), memory_permission::read_write);
            this->emu->memory.write_memory(view, data.data(), data.size());
            this->emu->memory.write_memory(this->storage + 8, uint32_t{4});
            this->emu->memory.write_memory(this->storage + 64, static_cast<uint64_t>(data.size()));
            const auto result = this->request(72, view, data.size());
            ASSERT_EQ(STATUS_SUCCESS, result.status);
            EXPECT_EQ(data, this->endpoint.received_data);
            EXPECT_EQ(0x1234u, this->endpoint.received_handle);
            ASSERT_TRUE(result.payload.has_value());
            uint32_t width{};
            std::memcpy(&width, result.payload->data() + 40, sizeof(width));
            EXPECT_EQ(syntax == 3 ? 8u : 4u, width);
        }
    }

    TEST_F(RpcBindingTest, RejectsMissingTruncatedOrOversizedRequestView)
    {
        this->bind(0, 1, 0x11);
        ASSERT_EQ(STATUS_SUCCESS, this->call(0).status);
        this->emu->memory.write_memory(this->storage + 8, uint32_t{4});
        this->emu->memory.write_memory(this->storage + 64, uint64_t{0x1001});
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->request(72).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->request(64, this->storage, 0x1000).status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->request(72, this->storage, 0x1000).status);
        EXPECT_TRUE(this->endpoint.received_data.empty());
    }

    TEST_F(RpcBindingTest, RejectsShortCallHeadersWithoutDispatching)
    {
        this->bind(0, 1, 0x11);
        ASSERT_EQ(STATUS_SUCCESS, this->call(0).status);
        for (ULONG size = 0; size < 64; ++size)
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->request(size).status) << size;
        }
        EXPECT_TRUE(this->endpoint.received_data.empty());
    }

    TEST_F(RpcBindingTest, RejectsUnmappedOperationAndPartiallyMappedCallHeader)
    {
        this->bind(0, 1, 0x11);
        ASSERT_EQ(STATUS_SUCCESS, this->call(0).status);
        const auto original_storage = this->storage;
        this->storage += 0x1000;
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->request(64).status);
        this->storage = original_storage + 0xffc;
        this->emu->memory.write_memory(this->storage, uint32_t{0});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->request(64).status);
        EXPECT_TRUE(this->endpoint.received_data.empty());
    }

    TEST_F(RpcBindingTest, RejectsUnmappedLargeRequestLength)
    {
        this->bind(0, 1, 0x11);
        ASSERT_EQ(STATUS_SUCCESS, this->call(0).status);
        this->storage += 0xfc0;
        this->emu->memory.set_memory(this->storage, 0, 64);
        this->emu->memory.write_memory(this->storage + 8, uint32_t{4});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->request(72, this->storage, 0x1000).status);
        EXPECT_TRUE(this->endpoint.received_data.empty());
    }

    TEST_F(RpcBindingTest, RejectsPartiallyMappedHandshakeWithoutReplacingBinding)
    {
        this->bind(0, 1, 0x11);
        const auto original_storage = this->storage;
        this->storage += 0xfe0;
        this->emu->memory.set_memory(this->storage, 0, 32);
        this->emu->memory.write_memory(this->storage, uint32_t{1});
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->request(72).status);
        this->storage = original_storage;
        this->expect_call(0, 0x11, 4);
    }

    TEST_F(RpcBindingTest, RequestSerializationPreservesViewAndTransferredHandle)
    {
        const lpc_request_context source{
            .send_buffer = 0x1000, .send_buffer_length = 72, .send_handle = 0x1234, .send_view_base = 0x2000, .send_view_size = 0x5000};
        utils::buffer_serializer snapshot;
        source.serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        lpc_request_context restored{};
        restored.deserialize(input);
        EXPECT_EQ(source.send_handle, restored.send_handle);
        EXPECT_EQ(source.send_view_base, restored.send_view_base);
        EXPECT_EQ(source.send_view_size, restored.send_view_size);
    }
}
