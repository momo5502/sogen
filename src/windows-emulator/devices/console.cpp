#include "../std_include.hpp"
#include "console.hpp"

#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        constexpr ULONG console_ioctl = 0x500016;

        enum class console_api : uint32_t
        {
            fill_console_output = 0x02000000,
            set_console_cursor_position = 0x0200000A,
            set_console_text_attribute = 0x0200000D,
            get_console_screen_buffer_info = 0x02000007,
        };

        enum class fill_console_output_type : uint32_t
        {
            ansi_character = 1,
            unicode_character = 2,
            attribute = 3,
        };

        struct console_coordinate
        {
            int16_t x;
            int16_t y;
        };

        static_assert(sizeof(console_coordinate) == 4);

        struct fill_console_output_request
        {
            console_coordinate coordinate;
            fill_console_output_type type;
            uint16_t value;
            uint16_t padding;
            uint32_t count;
        };

        static_assert(sizeof(fill_console_output_request) == 16);

        struct console_screen_buffer_info_response
        {
            int16_t size_x;
            int16_t size_y;
            int16_t cursor_x;
            int16_t cursor_y;
            int16_t window_left;
            int16_t window_top;
            uint16_t attributes;
            int16_t window_width;
            int16_t window_height;
            int16_t maximum_window_width;
            int16_t maximum_window_height;
            uint16_t popup_attributes;
            uint8_t fullscreen_supported;
            std::array<uint32_t, 16> color_table;
        };

        static_assert(sizeof(console_screen_buffer_info_response) == 92);

        struct console_ioctl_header
        {
            uint64_t target_handle;
            uint32_t input_count;
            uint32_t output_count;
            uint32_t message_buffer_size;
            uint32_t padding;
            uint64_t message;
            uint64_t data_size;
            uint64_t data;
        };

        static_assert(sizeof(console_ioctl_header) == 48);

        struct console_message_header
        {
            uint32_t api_number;
            uint32_t data_size;
        };

    }

    namespace
    {
        struct console_device final : io_device
        {
            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                buffer.write(text_attributes_);
                buffer.write(cursor_position_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                buffer.read(text_attributes_);
                buffer.read(cursor_position_);
            }

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                if (context.io_control_code != console_ioctl)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                if (!context.input_buffer || context.input_buffer_length < sizeof(console_ioctl_header))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                console_ioctl_header header{};
                win_emu.emu().read_memory(context.input_buffer, &header, sizeof(header));
                if (header.target_handle != STDOUT_HANDLE.h || header.input_count != 1 || header.output_count != 1 ||
                    header.message_buffer_size != sizeof(console_message_header) + header.data_size ||
                    header.data != header.message + sizeof(console_message_header) || !header.message || !header.data)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                console_message_header message{};
                win_emu.emu().read_memory(header.message, &message, sizeof(message));
                if (message.data_size != header.data_size)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                switch (static_cast<console_api>(message.api_number))
                {
                case console_api::fill_console_output: {
                    if (message.data_size != sizeof(fill_console_output_request))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    fill_console_output_request request{};
                    win_emu.emu().read_memory(header.data, &request, sizeof(request));
                    switch (request.type)
                    {
                    case fill_console_output_type::ansi_character:
                    case fill_console_output_type::unicode_character:
                    case fill_console_output_type::attribute:
                        // TODO
                        win_emu.emu().write_memory(header.data, &request, sizeof(request));
                        return STATUS_SUCCESS;
                    }

                    return STATUS_INVALID_PARAMETER;
                }

                case console_api::set_console_cursor_position:
                    if (message.data_size != sizeof(cursor_position_))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    win_emu.emu().read_memory(header.data, &cursor_position_, sizeof(cursor_position_));
                    return STATUS_SUCCESS;

                case console_api::set_console_text_attribute: {
                    if (message.data_size != sizeof(text_attributes_))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    win_emu.emu().read_memory(header.data, &text_attributes_, sizeof(text_attributes_));
                    return STATUS_SUCCESS;
                }

                case console_api::get_console_screen_buffer_info: {
                    if (message.data_size != sizeof(console_screen_buffer_info_response))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    console_screen_buffer_info_response response{};
                    response.size_x = 80;
                    response.size_y = 25;
                    response.cursor_x = cursor_position_.x;
                    response.cursor_y = cursor_position_.y;
                    response.window_width = response.size_x;
                    response.window_height = response.size_y;
                    response.maximum_window_width = response.size_x;
                    response.maximum_window_height = response.size_y;
                    response.attributes = text_attributes_;
                    response.popup_attributes = 0xF5;
                    response.color_table = {0x00000000, 0x00800000, 0x00008000, 0x00808000, 0x00000080, 0x00800080, 0x00008080, 0x00C0C0C0,
                                            0x00808080, 0x00FF0000, 0x0000FF00, 0x00FFFF00, 0x000000FF, 0x00FF00FF, 0x0000FFFF, 0x00FFFFFF};
                    win_emu.emu().write_memory(header.data, &response, sizeof(response));

                    return STATUS_SUCCESS;
                }
                }

                return STATUS_INVALID_PARAMETER;
            }

          private:
            uint16_t text_attributes_{7};
            console_coordinate cursor_position_{};
        };
    }

    std::unique_ptr<io_device> create_console_device()
    {
        return std::make_unique<console_device>();
    }

} // namespace sogen
