#pragma once

#include "emulation_test_utils.hpp"
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
                emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                emulator_callbacks{}, std::move(interfaces));
            this->storage = this->emu->memory.allocate_memory(0x20000, memory_permission::read_write);
            constexpr std::array<uint8_t, 12> sid{1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
            this->emu->process.sid.assign(sid.begin(), sid.end());
        }

        void bind(std::u16string_view name, const std::array<uint8_t, 16>& interface_id, size_t width)
        {
            this->pointer_size = width;
            this->endpoint = create_port(name);
            this->emu->memory.set_memory(this->storage, 0, 72);
            this->emu->memory.write_memory(this->storage, uint32_t{1});
            this->emu->memory.write_memory(this->storage + 12, interface_id.data(), interface_id.size());
            this->emu->memory.write_memory(this->storage + 32, width == 8 ? uint32_t{3} : uint32_t{1});
            EXPECT_EQ(STATUS_SUCCESS, this->request(72).status);
        }

        lpc_request_result call(uint32_t procedure, std::span<const uint8_t> data)
        {
            this->emu->memory.set_memory(this->storage, 0, 64);
            this->emu->memory.write_memory(this->storage + 12, uint32_t{1});
            this->emu->memory.write_memory(this->storage + 20, procedure);
            if (!data.empty())
            {
                this->emu->memory.write_memory(this->storage + 64, data.data(), data.size());
            }
            return this->request(static_cast<ULONG>(64 + data.size()));
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

      private:
        lpc_request_result request(ULONG length)
        {
            return this->endpoint->handle_request(*this->emu, {.send_buffer = this->storage,
                                                               .send_buffer_length = length,
                                                               .recv_buffer = this->storage + 0x10000,
                                                               .recv_buffer_length = 0x10000});
        }
    };
}
