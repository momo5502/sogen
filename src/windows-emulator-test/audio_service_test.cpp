#include "rpc_test_utils.hpp"

#include <chrono>
#include <thread>

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> mmdevice_interface_20348{0x1f, 0x17, 0xbe, 0xa3, 0x25, 0xa5, 0xa3, 0x47,
                                                                   0xb7, 0x5a, 0x93, 0xda, 0x9d, 0x64, 0x5d, 0x1c};
        constexpr std::array<uint8_t, 16> audio_client_interface_20348{0x41, 0xc1, 0xb2, 0x98, 0xa2, 0xec, 0x19, 0x49,
                                                                       0x98, 0xb4, 0xa1, 0x98, 0x0d, 0x00, 0xd2, 0x34};
        constexpr std::array<uint8_t, 16> audio_client_interface_26100{0x11, 0xd1, 0x74, 0xd5, 0x26, 0x61, 0xd7, 0x49,
                                                                       0x9b, 0x86, 0x4d, 0xe6, 0xb6, 0x50, 0xd4, 0xfc};

        constexpr size_t reply_header_size = 0x18;
        constexpr uint32_t sample_rate = 44100;
        constexpr uint16_t block_align = 8;
        constexpr uint64_t one_second_hns = 10000000;

        std::array<uint8_t, 40> float_stereo_format()
        {
            std::array<uint8_t, 40> format{};
            const auto put16 = [&](const size_t offset, const uint16_t value) { std::memcpy(&format[offset], &value, sizeof(value)); };
            const auto put32 = [&](const size_t offset, const uint32_t value) { std::memcpy(&format[offset], &value, sizeof(value)); };
            put16(0, 0xFFFE);
            put16(2, 2);
            put32(4, sample_rate);
            put32(8, sample_rate * block_align);
            put16(12, block_align);
            put16(14, 32);
            put16(16, 22);
            put16(18, 32);
            put32(20, 3);
            constexpr std::array<uint8_t, 16> ieee_float{0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                         0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
            std::memcpy(&format[24], ieee_float.data(), ieee_float.size());
            return format;
        }

        class AudioServiceTest : public RpcProtocolTest
        {
          protected:
            static std::vector<uint8_t> request_with_format(const size_t format_offset, const size_t size)
            {
                std::vector<uint8_t> data(size);
                const auto format = float_stereo_format();
                std::memcpy(data.data() + format_offset, format.data(), format.size());
                return data;
            }

            static uint32_t trailing_hresult(const std::vector<uint8_t>& payload)
            {
                return read<uint32_t>(payload, payload.size() - sizeof(uint32_t));
            }

            std::array<uint8_t, 20> open_stream(const size_t request_size)
            {
                const auto reply = this->call(4, request_with_format(152, request_size));
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                std::array<uint8_t, 20> context{};
                if (!reply.payload)
                {
                    ADD_FAILURE() << "Open stream returned no reply";
                    return context;
                }

                constexpr std::string_view uuid_prefix = "SogenAudioSt";
                const std::string_view bytes(reinterpret_cast<const char*>(reply.payload->data()), reply.payload->size());
                const auto uuid = bytes.find(uuid_prefix);
                if (uuid == std::string_view::npos || uuid + 16 > bytes.size())
                {
                    ADD_FAILURE() << "Open stream reply carries no stream context";
                    return context;
                }

                std::memcpy(context.data() + 4, reply.payload->data() + uuid, 16);
                return context;
            }

            uint64_t create_stream(const std::array<uint8_t, 20>& context)
            {
                std::vector<uint8_t> data(44);
                std::memcpy(data.data(), context.data(), context.size());
                std::memcpy(data.data() + 24, &one_second_hns, sizeof(one_second_hns));

                const auto reply = this->call(7, data);
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                if (reply.handles.size() != 1)
                {
                    ADD_FAILURE() << "Create stream delivered " << reply.handles.size() << " handles";
                    return 0;
                }

                const auto* render_section = this->emu->process.sections.get(make_handle(reply.handles.front().handle));
                if (!render_section)
                {
                    ADD_FAILURE() << "Create stream delivered no render section";
                    return 0;
                }

                return render_section->backing_address;
            }

            uint64_t create_render_buffer(const std::array<uint8_t, 16>& interface_id)
            {
                this->bind(u"\\RPC Control\\AudioClientRpc", interface_id, 8);
                return this->create_stream(this->open_stream(368));
            }

            void expect_format_accepted(const std::array<uint8_t, 16>& interface_id, const size_t width, const size_t format_offset,
                                        const size_t request_size)
            {
                this->bind(u"\\RPC Control\\AudioClientRpc", interface_id, width);
                const auto reply = this->call(1, request_with_format(format_offset, request_size));
                ASSERT_EQ(STATUS_SUCCESS, reply.status);
                ASSERT_TRUE(reply.payload.has_value());
                EXPECT_EQ(reply_header_size + width + sizeof(uint32_t), reply.payload->size()) << width;
                EXPECT_EQ(0u, trailing_hresult(*reply.payload)) << width;
            }

            void expect_session_opnums(const std::array<uint8_t, 16>& interface_id, const uint32_t get_state, const uint32_t destroy,
                                       const uint32_t derive_category)
            {
                this->bind(u"\\RPC Control\\AudioClientRpc", interface_id, 8);
                const std::vector<uint8_t> session(20);

                const auto state = this->call(get_state, session);
                ASSERT_EQ(STATUS_SUCCESS, state.status);
                ASSERT_TRUE(state.payload.has_value());
                EXPECT_EQ(reply_header_size + 28, state.payload->size());
                EXPECT_EQ(0u, trailing_hresult(*state.payload));

                const auto destroyed = this->call(destroy, session);
                ASSERT_EQ(STATUS_SUCCESS, destroyed.status);
                ASSERT_TRUE(destroyed.payload.has_value());
                EXPECT_EQ(reply_header_size + 24, destroyed.payload->size());

                const std::vector<uint8_t> category{3, 0, 0, 0, 0, 0, 0, 0};
                const auto derived = this->call(derive_category, category);
                ASSERT_EQ(STATUS_SUCCESS, derived.status);
                ASSERT_TRUE(derived.payload.has_value());
                EXPECT_EQ(3u, read<uint32_t>(*derived.payload, reply_header_size));
                EXPECT_EQ(0u, trailing_hresult(*derived.payload));
            }

            template <typename T>
            T read_guest(const uint64_t address)
            {
                return this->emu->memory.read_memory<T>(address);
            }

            bool play_cursor_reaches(const uint64_t play_cursor, const uint64_t value)
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (this->read_guest<uint64_t>(play_cursor) == value)
                    {
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
            }
        };
    }

    TEST_F(AudioServiceTest, ServesServer2022EndpointEnumerator)
    {
        this->bind(u"\\RPC Control\\Audiosrv", mmdevice_interface_20348, 8);
        const auto reply = this->call(0, {});
        ASSERT_EQ(STATUS_SUCCESS, reply.status);
        ASSERT_TRUE(reply.payload.has_value());
        EXPECT_EQ(0u, read<uint64_t>(*reply.payload, reply_header_size));
        EXPECT_EQ(0u, trailing_hresult(*reply.payload));
    }

    TEST_F(AudioServiceTest, ReadsServer2022IsFormatSupportedFormat)
    {
        this->expect_format_accepted(audio_client_interface_20348, 8, 312, 368);
        this->expect_format_accepted(audio_client_interface_20348, 4, 284, 332);
    }

    TEST_F(AudioServiceTest, ReadsServer2025IsFormatSupportedFormat)
    {
        this->expect_format_accepted(audio_client_interface_26100, 8, 336, 392);
        this->expect_format_accepted(audio_client_interface_26100, 4, 308, 356);
    }

    TEST_F(AudioServiceTest, AnswersServer2022SessionOpnums)
    {
        this->expect_session_opnums(audio_client_interface_20348, 26, 54, 120);
    }

    TEST_F(AudioServiceTest, AnswersServer2025SessionOpnums)
    {
        this->expect_session_opnums(audio_client_interface_26100, 27, 55, 122);
    }

    TEST_F(AudioServiceTest, CreatesServer2022RenderControlData)
    {
        const auto header = this->create_render_buffer(audio_client_interface_20348);
        ASSERT_NE(0u, header);

        EXPECT_EQ(0x45504344u, this->read_guest<uint32_t>(header + 0xC0));
        EXPECT_EQ(22u + 200u, this->read_guest<uint32_t>(header + 0xC8));
        EXPECT_EQ(uint16_t{0xFFFE}, this->read_guest<uint16_t>(header + 0x17C));
        EXPECT_EQ(sample_rate, this->read_guest<uint32_t>(header + 0x180));
        const auto data_start = this->read_guest<uint32_t>(header + 0x168);
        const auto data_end = this->read_guest<uint32_t>(header + 0x16C);
        EXPECT_EQ(0x400u, data_start);
        EXPECT_EQ(uint32_t{sample_rate} * block_align, data_end - data_start);
        EXPECT_LE(data_end, this->read_guest<uint32_t>(header + 0x170));

        this->emu->memory.write_memory(header + 0x10, uint64_t{4096});
        EXPECT_TRUE(this->play_cursor_reaches(header + 0x18, 4096));
    }

    TEST_F(AudioServiceTest, KeepsServer2025RenderControlData)
    {
        const auto header = this->create_render_buffer(audio_client_interface_26100);
        ASSERT_NE(0u, header);

        EXPECT_EQ(1u, this->read_guest<uint32_t>(header));
        EXPECT_EQ(0x45504344u, this->read_guest<uint32_t>(header + 0xC8));
        EXPECT_EQ(uint16_t{0xFFFE}, this->read_guest<uint16_t>(header + 0x180));

        this->emu->memory.write_memory(header + 0x18, uint64_t{4096});
        EXPECT_TRUE(this->play_cursor_reaches(header + 0x20, 4096));
    }
}
