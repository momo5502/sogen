#include "../std_include.hpp"
#include "console.hpp"

#include "../windows_emulator.hpp"
#include <platform/console_backend.hpp>

namespace sogen
{

    namespace
    {
        constexpr ULONG console_ioctl = 0x500016;

        enum class console_api : uint32_t
        {
            get_console_code_page = 0x01000000,
            get_console_mode = 0x01000001,
            set_console_mode = 0x01000002,
            get_console_input_event_count = 0x01000003,
            read_console_input = 0x01000004,
            write_console = 0x01000006,
            get_console_locale = 0x01000008,
            fill_console_output = 0x02000000,
            set_console_cursor_position = 0x0200000A,
            set_console_text_attribute = 0x0200000D,
            get_console_screen_buffer_info = 0x02000007,
        };

        constexpr uint32_t default_input_mode = 0x007F;
        constexpr uint32_t default_output_mode = 0x0003;

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

        struct console_ioctl_input_header
        {
            console_ioctl_header header;
            uint32_t output_buffer_size;
            uint32_t padding;
            uint64_t output_buffer;
        };

        static_assert(sizeof(console_ioctl_input_header) == 64);

        struct read_console_input_request
        {
            uint32_t events_read;
            uint16_t flags;
            uint8_t unicode;
            uint8_t padding;
        };

        static_assert(sizeof(read_console_input_request) == 8);

        struct write_console_request
        {
            uint32_t characters_written;
            uint8_t unicode;
            std::array<uint8_t, 3> padding;
        };

        static_assert(sizeof(write_console_request) == 8);

        struct console_input_record
        {
            uint16_t event_type;
            uint16_t padding;
            int32_t key_down;
            uint16_t repeat_count;
            uint16_t virtual_key_code;
            uint16_t virtual_scan_code;
            char16_t unicode_character;
            uint32_t control_key_state;
        };

        static_assert(sizeof(console_input_record) == 20);

        struct console_message_header
        {
            uint32_t api_number;
            uint32_t data_size;
        };

        void set_console_key(console_input_record& record, const uint16_t virtual_key, const uint16_t scan_code,
                             const bool enhanced = false)
        {
            record.virtual_key_code = virtual_key;
            record.virtual_scan_code = scan_code;
            if (enhanced)
            {
                record.control_key_state |= 0x0100;
            }
        }

        console_input_record make_console_input_record(const console_key_event& event)
        {
            console_input_record record{};
            record.event_type = 1;
            record.key_down = 1;
            record.repeat_count = 1;
            record.unicode_character = event.character;

            switch (event.key)
            {
            case console_key::character:
                if (event.control && event.character > 0 && event.character <= 0x1A)
                {
                    record.virtual_key_code = static_cast<uint16_t>('A' + event.character - 1);
                    record.control_key_state = 0x0008;
                }
                else if (event.character >= 'a' && event.character <= 'z')
                {
                    record.virtual_key_code = static_cast<uint16_t>('A' + event.character - 'a');
                }
                else
                {
                    record.virtual_key_code = event.character;
                }
                break;
            case console_key::escape:
                set_console_key(record, VK_ESCAPE, 0x01);
                break;
            case console_key::enter:
                set_console_key(record, VK_RETURN, 0x1C);
                break;
            case console_key::tab:
                set_console_key(record, VK_TAB, 0x0F);
                break;
            case console_key::backspace:
                set_console_key(record, VK_BACK, 0x0E);
                break;
            case console_key::up:
                set_console_key(record, VK_UP, 0x48, true);
                break;
            case console_key::down:
                set_console_key(record, VK_DOWN, 0x50, true);
                break;
            case console_key::left:
                set_console_key(record, VK_LEFT, 0x4B, true);
                break;
            case console_key::right:
                set_console_key(record, VK_RIGHT, 0x4D, true);
                break;
            case console_key::home:
                set_console_key(record, VK_HOME, 0x47, true);
                break;
            case console_key::end:
                set_console_key(record, VK_END, 0x4F, true);
                break;
            case console_key::insert:
                set_console_key(record, VK_INSERT, 0x52, true);
                break;
            case console_key::delete_key:
                set_console_key(record, VK_DELETE, 0x53, true);
                break;
            case console_key::page_up:
                set_console_key(record, VK_PRIOR, 0x49, true);
                break;
            case console_key::page_down:
                set_console_key(record, VK_NEXT, 0x51, true);
                break;
            }

            return record;
        }

        handle effective_console_endpoint(const io_device_context& context, const uint64_t target_handle)
        {
            return target_handle == 0 ? context.source_handle : make_handle(target_handle);
        }

        bool is_input_endpoint(const handle endpoint)
        {
            return endpoint == STDIN_HANDLE;
        }

        console_input_mode make_console_input_mode(const uint32_t mode)
        {
            return {
                .processed = (mode & 0x0001) != 0,
                .line = (mode & 0x0002) != 0,
                .echo = (mode & 0x0004) != 0,
            };
        }

    }

    namespace
    {
        struct console_device final : io_device
        {
            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                buffer.write(text_attributes_);
                buffer.write(cursor_position_);
                buffer.write(input_mode_);
                buffer.write(output_mode_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                buffer.read(text_attributes_);
                buffer.read(cursor_position_);
                buffer.read(input_mode_);
                buffer.read(output_mode_);
            }

            void restore_after_state_restore(windows_emulator& win_emu) override
            {
                win_emu.console().set_input_mode(make_console_input_mode(input_mode_));
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
                if (header.target_handle != STDIN_HANDLE.h && header.target_handle != STDOUT_HANDLE.h &&
                    header.target_handle != CONSOLE_HANDLE.h && header.target_handle != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const auto endpoint = effective_console_endpoint(context, header.target_handle);

                if (header.message_buffer_size != sizeof(console_message_header) + header.data_size ||
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
                case console_api::get_console_mode: {
                    if (message.data_size != sizeof(uint32_t))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const auto mode = is_input_endpoint(endpoint) ? input_mode_ : output_mode_;
                    win_emu.emu().write_memory(header.data, &mode, sizeof(mode));
                    return STATUS_SUCCESS;
                }

                case console_api::set_console_mode: {
                    if (message.data_size != sizeof(uint32_t))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    uint32_t mode{};
                    win_emu.emu().read_memory(header.data, &mode, sizeof(mode));
                    if (is_input_endpoint(endpoint))
                    {
                        input_mode_ = mode;
                        win_emu.console().set_input_mode(make_console_input_mode(mode));
                    }
                    else
                    {
                        output_mode_ = mode;
                    }
                    return STATUS_SUCCESS;
                }

                case console_api::get_console_code_page: {
                    if (message.data_size != sizeof(uint64_t))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    constexpr uint64_t utf8_code_page = 65001;
                    win_emu.emu().write_memory(header.data, &utf8_code_page, sizeof(utf8_code_page));
                    return STATUS_SUCCESS;
                }
                case console_api::get_console_locale: {
                    if (header.input_count != 1 || header.output_count != 1 || message.data_size != sizeof(uint16_t))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    constexpr uint16_t english_united_states = 0x0409;
                    win_emu.emu().write_memory(header.data, &english_united_states, sizeof(english_united_states));
                    return STATUS_SUCCESS;
                }

                case console_api::get_console_input_event_count: {
                    if (!is_input_endpoint(endpoint) || header.input_count != 1 || header.output_count != 1 ||
                        message.data_size != sizeof(uint32_t))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const uint32_t event_count = win_emu.console().input_available() ? 1 : 0;
                    win_emu.emu().write_memory(header.data, &event_count, sizeof(event_count));
                    return STATUS_SUCCESS;
                }
                case console_api::read_console_input: {
                    if (!is_input_endpoint(endpoint) || context.input_buffer_length < sizeof(console_ioctl_input_header) ||
                        header.input_count != 1 || header.output_count != 2 || message.data_size != sizeof(read_console_input_request))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    console_ioctl_input_header input_header{};
                    win_emu.emu().read_memory(context.input_buffer, &input_header, sizeof(input_header));
                    if (!input_header.output_buffer || input_header.output_buffer_size < sizeof(console_input_record))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const auto event = win_emu.console().read_input_event();
                    if (!event)
                    {
                        return STATUS_END_OF_FILE;
                    }

                    const auto record = make_console_input_record(*event);

                    auto request = win_emu.emu().read_memory<read_console_input_request>(header.data);
                    request.events_read = 1;
                    win_emu.emu().write_memory(header.data, &request, sizeof(request));
                    win_emu.emu().write_memory(input_header.output_buffer, &record, sizeof(record));
                    return STATUS_SUCCESS;
                }
                case console_api::write_console: {
                    if (is_input_endpoint(endpoint) || context.input_buffer_length < sizeof(console_ioctl_input_header) ||
                        header.input_count != 2 || header.output_count != 1 || message.data_size != sizeof(write_console_request))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    console_ioctl_input_header input_header{};
                    win_emu.emu().read_memory(context.input_buffer, &input_header, sizeof(input_header));
                    auto request = win_emu.emu().read_memory<write_console_request>(header.data);
                    if (!input_header.output_buffer || (request.unicode && input_header.output_buffer_size % sizeof(char16_t) != 0))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    if (request.unicode)
                    {
                        std::u16string output(input_header.output_buffer_size / sizeof(char16_t), u'\0');
                        win_emu.emu().read_memory(input_header.output_buffer, output.data(), input_header.output_buffer_size);
                        win_emu.callbacks.on_stdout(u16_to_u8(output));
                        request.characters_written = static_cast<uint32_t>(output.size());
                    }
                    else
                    {
                        std::string output(input_header.output_buffer_size, '\0');
                        win_emu.emu().read_memory(input_header.output_buffer, output.data(), output.size());
                        win_emu.callbacks.on_stdout(output);
                        request.characters_written = static_cast<uint32_t>(output.size());
                    }

                    win_emu.emu().write_memory(header.data, &request, sizeof(request));
                    return STATUS_SUCCESS;
                }

                case console_api::fill_console_output: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(fill_console_output_request))
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
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(cursor_position_))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    win_emu.emu().read_memory(header.data, &cursor_position_, sizeof(cursor_position_));
                    return STATUS_SUCCESS;

                case console_api::set_console_text_attribute: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(text_attributes_))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    win_emu.emu().read_memory(header.data, &text_attributes_, sizeof(text_attributes_));
                    return STATUS_SUCCESS;
                }

                case console_api::get_console_screen_buffer_info: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(console_screen_buffer_info_response))
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
                default:
                    break;
                }

                return STATUS_INVALID_PARAMETER;
            }

          private:
            uint16_t text_attributes_{7};
            uint32_t input_mode_{default_input_mode};
            uint32_t output_mode_{default_output_mode};
            console_coordinate cursor_position_{};
        };
    }

    std::unique_ptr<io_device> create_console_device(const device_creation_context&)
    {
        return std::make_unique<console_device>();
    }

} // namespace sogen
