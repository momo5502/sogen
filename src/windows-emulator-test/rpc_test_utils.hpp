#pragma once

#include <binary_writer.hpp>
#include <port.hpp>

namespace sogen::test
{
    class RpcProtocolTest : public testing::Test
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
                emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = this->load_registry},
                emulator_callbacks{}, std::move(interfaces));
            this->storage = this->emu->memory.allocate_memory(0x20000, memory_permission::read_write);
            constexpr std::array<uint8_t, 12> sid{1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
            this->emu->process.sid.assign(sid.begin(), sid.end());
        }

        void bind(std::u16string_view name, const std::array<uint8_t, 16>& interface_id, size_t width)
        {
            this->bind(create_port(name), interface_id, width);
        }

        void bind(std::unique_ptr<port> port, const std::array<uint8_t, 16>& interface_id, size_t width)
        {
            this->pointer_size = width;
            this->endpoint = std::move(port);
            this->emu->memory.set_memory(this->storage, 0, 72);
            this->emu->memory.write_memory(this->storage, uint32_t{1});
            this->emu->memory.write_memory(this->storage + 12, interface_id.data(), interface_id.size());
            this->emu->memory.write_memory(this->storage + 32, width == 8 ? uint32_t{3} : uint32_t{1});
            EXPECT_EQ(STATUS_SUCCESS, this->request(72).status);
        }

        // A caller that does not wait passes no reply buffer, which lets the service answer later.
        lpc_request_result call(uint32_t procedure, std::span<const uint8_t> data, const bool wait_for_reply = true)
        {
            this->emu->memory.set_memory(this->storage, 0, 64);
            this->emu->memory.write_memory(this->storage + 12, uint32_t{1});
            this->emu->memory.write_memory(this->storage + 20, procedure);
            if (!data.empty())
            {
                this->emu->memory.write_memory(this->storage + 64, data.data(), data.size());
            }
            return this->request(static_cast<ULONG>(64 + data.size()), wait_for_reply);
        }

        // Sends the call as an ALPC request message, the way NtAlpcSendWaitReceivePort hands it to the port. A sender that
        // does not wait can have its reply deferred only when a completion port is there to deliver it.
        lpc_message_result send_message(const uint32_t procedure, const std::span<const uint8_t> data, const bool wait_for_reply = true,
                                        const bool has_completion_port = true)
        {
            constexpr uint64_t send_offset = 0x8000;
            constexpr uint64_t receive_offset = 0xC000;
            constexpr size_t receive_length = 0x4000;
            constexpr size_t rpc_request_header_size = 64;

            std::vector<uint8_t> body(rpc_request_header_size + data.size());
            const auto call_id = ++this->last_call_id;
            std::memcpy(body.data() + 12, &call_id, sizeof(call_id));
            std::memcpy(body.data() + 20, &procedure, sizeof(procedure));
            std::ranges::copy(data, body.begin() + rpc_request_header_size);

            PORT_MESSAGE64 header{};
            header.u1.s1.DataLength = static_cast<CSHORT>(body.size());
            header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(header) + body.size());
            header.u2.s2.Type = LPC_REQUEST;
            this->emu->memory.write_memory(this->storage + send_offset, &header, sizeof(header));
            this->emu->memory.write_memory(this->storage + send_offset + sizeof(header), body.data(), body.size());

            lpc_message_context context{this->emu->memory};
            context.send_message.set_address(this->storage + send_offset);
            if (wait_for_reply)
            {
                context.receive_message.set_address(this->storage + receive_offset);
                context.receive_buffer_length = receive_length;
            }
            return this->endpoint->handle_message(*this->emu, context, !wait_for_reply && has_completion_port);
        }

        template <typename T>
        static T read(std::span<const uint8_t> data, size_t offset)
        {
            T value{};
            if (offset > data.size() || sizeof(T) > data.size() - offset)
            {
                ADD_FAILURE() << "Truncated RPC reply at " << offset;
                return value;
            }
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        static void write_unicode_string(utils::aligned_binary_writer& writer, std::u16string_view value)
        {
            writer.align_to(writer.pointer_size());
            writer.write(static_cast<uint16_t>(value.size() * sizeof(char16_t)));
            writer.write(static_cast<uint16_t>(value.size() * sizeof(char16_t)));
            writer.write_ndr_pointer(true);
            writer.write_ndr_u16string(value, false);
        }

        std::unique_ptr<windows_emulator> emu{};
        std::unique_ptr<port> endpoint{};
        uint64_t storage{};
        size_t pointer_size{};
        uint32_t last_call_id{};
        // A fixture whose service reads the registry sets this in its constructor to load the root's hives.
        bool load_registry{};

      private:
        lpc_request_result request(ULONG length, const bool wait_for_reply = true)
        {
            return this->endpoint->handle_request(*this->emu, {.send_buffer = this->storage,
                                                               .send_buffer_length = length,
                                                               .recv_buffer = wait_for_reply ? this->storage + 0x10000 : 0,
                                                               .recv_buffer_length = wait_for_reply ? 0x10000u : 0u});
        }
    };
}
