#include "../std_include.hpp"
#include "console.hpp"

#include "../windows_emulator.hpp"
#include "../io_completion_wait.hpp"
#include <platform/console_backend.hpp>
#include <utils/finally.hpp>

namespace sogen
{

    namespace
    {
        constexpr ULONG console_ioctl = 0x500016;
        constexpr uint16_t console_read_noremove = 0x0001;
        constexpr uint16_t console_read_nowait = 0x0002;

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
            get_console_cursor_info = 0x02000005,
            set_console_cursor_position = 0x0200000A,
            set_console_text_attribute = 0x0200000D,
            get_console_title = 0x02000014,
            set_console_title = 0x02000015,
            get_console_screen_buffer_info = 0x02000007,
            get_largest_console_window_size = 0x0200000B,
            get_current_console_font = 0x03000004,
            set_current_console_font = 0x0300002C,
        };

        constexpr uint32_t default_input_mode = 0x00F7;
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

        struct console_cursor_info_response
        {
            uint32_t size;
            uint32_t visible;
        };

        static_assert(sizeof(console_cursor_info_response) == 8);

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

        struct console_font_info_response
        {
            uint8_t maximum_window;
            std::array<uint8_t, 3> padding;
            uint32_t index;
            console_coordinate size;
            uint32_t family;
            uint32_t weight;
            std::array<char16_t, 32> face_name;
        };

        static_assert(sizeof(console_font_info_response) == 84);

        console_font_info_response default_console_font()
        {
            console_font_info_response font{};
            font.size = {.x = 8, .y = 16};
            font.family = 0x36;
            font.weight = 400;
            constexpr std::u16string_view face = u"Consolas";
            std::ranges::copy(face, font.face_name.begin());
            return font;
        }

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
            uint32_t bytes_written;
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
                else if ((event.character >= 'A' && event.character <= 'Z') || (event.character >= '0' && event.character <= '9') ||
                         event.character == u' ')
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

            if (event.control)
            {
                record.control_key_state |= 0x0008;
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
        struct pending_read
        {
            io_device_context context;
            uint64_t request_address{};
            uint64_t output_address{};
            read_console_input_request request{};
            object_reference issuing_thread{};
            bool text{};
            bool cancelled{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(context);
                buffer.write(request_address);
                buffer.write(output_address);
                buffer.write(request);
                buffer.write(issuing_thread);
                buffer.write(text);
                buffer.write(cancelled);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(context);
                buffer.read(request_address);
                buffer.read(output_address);
                buffer.read(request);
                buffer.read(issuing_thread);
                buffer.read(text);
                buffer.read(cancelled);
            }
        };

        struct console_device final : io_device
        {
            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                buffer.write(text_attributes_);
                buffer.write(cursor_position_);
                buffer.write(input_mode_);
                buffer.write(output_mode_);
                buffer.write(font_info_);
                buffer.write_string(title_);
                buffer.write_string(original_title_);
                buffer.write(static_cast<uint64_t>(pending_reads_.size()));
                for (const auto& read : pending_reads_)
                {
                    buffer.write(read);
                }
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                buffer.read(text_attributes_);
                buffer.read(cursor_position_);
                buffer.read(input_mode_);
                buffer.read(output_mode_);
                buffer.read(font_info_);
                buffer.read_string(title_);
                buffer.read_string(original_title_);
                pending_reads_.clear();
                const auto count = buffer.read<uint64_t>();
                for (uint64_t i = 0; i < count; ++i)
                {
                    pending_read read{.context = io_device_context{buffer}};
                    buffer.read(read);
                    pending_reads_.push_back(read);
                }
            }

            bool does_work() const override
            {
                return true;
            }

            void work(windows_emulator& win_emu) override
            {
                for (auto it = pending_reads_.begin(); it != pending_reads_.end();)
                {
                    if (it->cancelled)
                    {
                        const auto first_text = std::ranges::find_if(pending_reads_, [](const auto& read) { return read.text; });
                        if (it->text && it == first_text && !win_emu.console().cancel_read_input())
                        {
                            ++it;
                            continue;
                        }
                        it = finish_read(win_emu, it, STATUS_CANCELLED);
                        continue;
                    }
                    if (it->text)
                    {
                        const auto first_text = std::ranges::find_if(pending_reads_, [](const auto& read) { return read.text; });
                        if (it != first_text && it->context.output_buffer_length != 0)
                        {
                            ++it;
                            continue;
                        }
                        const auto data = win_emu.console().try_read_input(it->context.output_buffer_length);
                        if (!data)
                        {
                            ++it;
                            continue;
                        }
                        const auto status =
                            data->empty() || win_emu.memory.try_write_memory(it->context.output_buffer, data->data(), data->size())
                                ? STATUS_SUCCESS
                                : STATUS_ACCESS_VIOLATION;
                        it = finish_read(win_emu, it, status, status == STATUS_SUCCESS ? data->size() : 0);
                        continue;
                    }
                    const auto event = win_emu.console().read_input_event(false, (it->request.flags & console_read_noremove) == 0);
                    if (!event && !win_emu.console().input_closed())
                    {
                        ++it;
                        continue;
                    }
                    it->request.events_read = 0;
                    NTSTATUS status{};
                    if (event)
                    {
                        status = write_read_result(win_emu, *it, *event);
                    }
                    else
                    {
                        status = win_emu.memory.try_write_memory(it->request_address, &it->request, sizeof(it->request))
                                     ? STATUS_END_OF_FILE
                                     : STATUS_ACCESS_VIOLATION;
                    }
                    it = finish_read(win_emu, it, status);
                }
            }

            bool has_pending_io(const uint32_t issuing_thread_id) const override
            {
                return std::ranges::any_of(pending_reads_,
                                           [=](const auto& read) { return read.context.issuing_thread_id == issuing_thread_id; });
            }

            bool cancel_io(windows_emulator& win_emu, const uint64_t io_status_block,
                           const std::optional<uint32_t> issuing_thread_id) override
            {
                bool cancelled{};
                for (auto it = pending_reads_.begin(); it != pending_reads_.end();)
                {
                    if ((issuing_thread_id && it->context.issuing_thread_id != *issuing_thread_id) ||
                        (io_status_block && it->context.io_status_block.value() != io_status_block &&
                         it->context.wow64_io_status_block.value() != io_status_block))
                    {
                        ++it;
                        continue;
                    }
                    if (it->text)
                    {
                        it->cancelled = true;
                        const auto first_text = std::ranges::find_if(pending_reads_, [](const auto& read) { return read.text; });
                        if (it == first_text && !win_emu.console().cancel_read_input())
                        {
                            cancelled = true;
                            ++it;
                            continue;
                        }
                    }
                    it = finish_read(win_emu, it, STATUS_CANCELLED);
                    cancelled = true;
                }
                return cancelled;
            }

            void release_references(process_context& process) override
            {
                for (auto& read : pending_reads_)
                {
                    release_read(process, read);
                }
                pending_reads_.clear();
            }

            void restore_after_state_restore(windows_emulator& win_emu) override
            {
                win_emu.console().set_input_mode(make_console_input_mode(input_mode_));
            }

            NTSTATUS read_input(windows_emulator& win_emu, const io_device_context& context)
            {
                return queue_read(win_emu, pending_read{.context = context, .text = true});
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

                if (!header.message || header.message_buffer_size < sizeof(console_message_header))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                console_message_header message{};
                win_emu.emu().read_memory(header.message, &message, sizeof(message));
                if ((message.api_number == static_cast<uint32_t>(console_api::write_console) ||
                     message.api_number == static_cast<uint32_t>(console_api::set_console_title)) &&
                    header.input_count == 2 && header.output_count == 1)
                {
                    if (context.input_buffer_length < sizeof(console_ioctl_input_header))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    const auto write_header = win_emu.emu().read_memory<console_ioctl_input_header>(context.input_buffer);
                    header.data_size = write_header.output_buffer_size;
                    header.data = write_header.output_buffer;
                }
                if (header.message_buffer_size != sizeof(console_message_header) + header.data_size ||
                    header.data != header.message + sizeof(console_message_header) || !header.data || message.data_size != header.data_size)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                switch (static_cast<console_api>(message.api_number))
                {
                case console_api::set_console_title: {
                    if (context.input_buffer_length < sizeof(console_ioctl_input_header) || header.input_count != 2 ||
                        header.output_count != 1 || message.data_size != sizeof(uint8_t))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    const auto input_header = win_emu.emu().read_memory<console_ioctl_input_header>(context.input_buffer);
                    const auto unicode = win_emu.emu().read_memory<uint8_t>(header.data) != 0;
                    const auto text_size = input_header.header.data_size;
                    const auto text_address = input_header.header.data;
                    if (text_size >= 65536 || (!text_address && text_size != 0) || (unicode && text_size % sizeof(char16_t) != 0))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    if (unicode)
                    {
                        std::u16string title(static_cast<size_t>(text_size / sizeof(char16_t)), u'\0');
                        if (text_size != 0 && !win_emu.emu().try_read_memory(text_address, title.data(), static_cast<size_t>(text_size)))
                        {
                            return STATUS_ACCESS_VIOLATION;
                        }
                        this->title_ = std::move(title);
                    }
                    else
                    {
                        std::string title(static_cast<size_t>(text_size), '\0');
                        if (text_size != 0 && !win_emu.emu().try_read_memory(text_address, title.data(), title.size()))
                        {
                            return STATUS_ACCESS_VIOLATION;
                        }
                        this->title_ = u8_to_u16(title);
                    }
                    return STATUS_SUCCESS;
                }
                case console_api::get_console_title: {
                    struct title_request
                    {
                        uint32_t length;
                        uint8_t unicode;
                        uint8_t original;
                        std::array<uint8_t, 2> padding;
                    };

                    if (context.input_buffer_length < sizeof(console_ioctl_input_header) || header.input_count != 1 ||
                        header.output_count != 2 || message.data_size != sizeof(title_request))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    const auto input_header = win_emu.emu().read_memory<console_ioctl_input_header>(context.input_buffer);
                    auto request = win_emu.emu().read_memory<title_request>(header.data);
                    const auto& title = request.original ? this->original_title_ : this->title_;
                    size_t written{};
                    const auto copy_title = [&]<typename Char>(const std::basic_string<Char>& value) {
                        const auto capacity = input_header.output_buffer_size / sizeof(Char);
                        request.length = static_cast<uint32_t>(value.size());
                        if (capacity == 0)
                        {
                            return true;
                        }
                        auto output = value.substr(0, std::min(value.size(), static_cast<size_t>(capacity - 1)));
                        written = output.size() * sizeof(Char);
                        output.push_back(Char{});
                        return win_emu.emu().try_write_memory(input_header.output_buffer, output.data(), output.size() * sizeof(Char));
                    };
                    if (!(request.unicode ? copy_title(title) : copy_title(u16_to_u8(title))))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    win_emu.emu().write_memory(header.data, request);
                    if (context.io_status_block)
                    {
                        context.io_status_block.access([&](auto& status) { status.Information = written; });
                    }
                    if (context.wow64_io_status_block)
                    {
                        context.wow64_io_status_block.access([&](auto& status) { status.Information = static_cast<uint32_t>(written); });
                    }
                    return STATUS_SUCCESS;
                }
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

                    auto request = win_emu.emu().read_memory<read_console_input_request>(header.data);
                    if ((request.flags & ~(console_read_noremove | console_read_nowait)) != 0)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const auto wait = (request.flags & console_read_nowait) == 0;
                    const auto remove = (request.flags & console_read_noremove) == 0;
                    const auto event = win_emu.console().read_input_event(false, remove);
                    if (!event)
                    {
                        if (wait && !win_emu.console().input_closed())
                        {
                            return queue_read(win_emu, context, header.data, input_header.output_buffer, request);
                        }
                        request.events_read = 0;
                        win_emu.emu().write_memory(header.data, &request, sizeof(request));
                        return wait ? STATUS_END_OF_FILE : STATUS_SUCCESS;
                    }

                    const auto record = make_console_input_record(*event);

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
                    const auto text_address = input_header.header.data;
                    const auto text_size = input_header.header.data_size;
                    if ((!text_address && text_size != 0) || text_size > std::numeric_limits<ULONG>::max() ||
                        (request.unicode && text_size % sizeof(char16_t) != 0))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    if (request.unicode)
                    {
                        std::u16string output(static_cast<size_t>(text_size / sizeof(char16_t)), u'\0');
                        win_emu.emu().read_memory(text_address, output.data(), static_cast<size_t>(text_size));
                        win_emu.callbacks.on_stdout(u16_to_u8(output));
                    }
                    else
                    {
                        std::string output(static_cast<size_t>(text_size), '\0');
                        win_emu.emu().read_memory(text_address, output.data(), output.size());
                        win_emu.callbacks.on_stdout(output);
                    }

                    request.bytes_written = static_cast<uint32_t>(text_size);
                    win_emu.emu().write_memory(header.data, &request, sizeof(request));
                    return STATUS_SUCCESS;
                }

                case console_api::fill_console_output: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(fill_console_output_request))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
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

                case console_api::get_console_cursor_info: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(console_cursor_info_response))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    constexpr console_cursor_info_response cursor_info{.size = 25, .visible = 1};
                    win_emu.emu().write_memory(header.data, cursor_info);
                    return STATUS_SUCCESS;
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

                case console_api::get_current_console_font: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(console_font_info_response))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    win_emu.emu().write_memory(header.data, font_info_);
                    return STATUS_SUCCESS;
                }

                case console_api::set_current_console_font: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(console_font_info_response))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    auto font = win_emu.emu().read_memory<console_font_info_response>(header.data);
                    if (font.size.x < 0 || font.size.y < 0)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    font.size.x = font.size.x == 0 ? font_info_.size.x : font.size.x;
                    font.size.y = font.size.y == 0 ? font_info_.size.y : font.size.y;
                    font.family = font.family == 0 ? font_info_.family : font.family;
                    font.weight = font.weight == 0 ? font_info_.weight : font.weight;
                    if (font.face_name.front() == u'\0')
                    {
                        font.face_name = font_info_.face_name;
                    }
                    font.face_name.back() = u'\0';
                    font_info_ = font;
                    win_emu.emu().write_memory(header.data, font_info_);
                    return STATUS_SUCCESS;
                }

                case console_api::get_largest_console_window_size: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(console_coordinate))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    const auto dimensions = win_emu.console().get_size();
                    const console_coordinate size{.x = dimensions.width, .y = dimensions.height};
                    win_emu.emu().write_memory(header.data, size);
                    return STATUS_SUCCESS;
                }

                case console_api::get_console_screen_buffer_info: {
                    if (is_input_endpoint(endpoint) || message.data_size != sizeof(console_screen_buffer_info_response))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    console_screen_buffer_info_response response{};
                    const auto dimensions = win_emu.console().get_size();
                    response.size_x = dimensions.width;
                    response.size_y = dimensions.height;
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
                    win_emu.log.warn("Unimplemented console API: 0x%08X\n", static_cast<unsigned>(message.api_number));
                    break;
                }

                return STATUS_INVALID_PARAMETER;
            }

          private:
            static NTSTATUS write_read_result(windows_emulator& win_emu, pending_read& read, const console_key_event& event)
            {
                const auto record = make_console_input_record(event);
                read.request.events_read = 1;
                if (!win_emu.memory.try_write_memory(read.output_address, &record, sizeof(record)) ||
                    !win_emu.memory.try_write_memory(read.request_address, &read.request, sizeof(read.request)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                return STATUS_SUCCESS;
            }

            static void complete_read(windows_emulator& win_emu, const pending_read& read, const NTSTATUS status,
                                      const uint64_t information = 0)
            {
                const auto& context = read.context;
                const IO_STATUS_BLOCK<EmulatorTraits<Emu64>> result{.Status = status, .Information = information};
                context.io_status_block.try_write(result);
                if (context.wow64_io_status_block)
                {
                    context.wow64_io_status_block.try_write(
                        {.Status = status, .Information = static_cast<EmulatorTraits<Emu32>::ULONG_PTR>(information)});
                }
                if (auto* event = win_emu.process.get_object_as<sogen::event>(context.event))
                {
                    win_emu.signal_event(*event);
                }
                if (context.apc_routine)
                {
                    if (auto* thread = win_emu.process.get_object_as<emulator_thread>(read.issuing_thread))
                    {
                        thread->queue_user_apc({.apc_routine = context.apc_routine,
                                                .apc_argument1 = context.apc_context,
                                                .apc_argument2 = context.wow64_io_status_block ? context.wow64_io_status_block.value()
                                                                                               : context.io_status_block.value(),
                                                .apc_argument3 = 0});
                    }
                }
                else
                {
                    queue_io_completion(win_emu, context, result);
                }
                if (!context.event && !context.apc_routine && !context.completion_port)
                {
                    if (auto* thread = win_emu.process.get_object_as<emulator_thread>(read.issuing_thread);
                        thread && thread->await_console_io)
                    {
                        thread->mark_as_ready(status);
                    }
                }
                win_emu.wake_idle_vcpu();
            }

            std::vector<pending_read>::iterator finish_read(windows_emulator& win_emu, std::vector<pending_read>::iterator read,
                                                            const NTSTATUS status, const uint64_t information = 0)
            {
                complete_read(win_emu, *read, status, information);
                release_read(win_emu.process, *read);
                return pending_reads_.erase(read);
            }

            static void release_read(process_context& process, pending_read& read)
            {
                io_completion_wait::release_reference(process, read.context.event);
                io_completion_wait::release_reference(process, read.context.completion_port);
                io_completion_wait::release_reference(process, read.issuing_thread);
            }

            NTSTATUS queue_read(windows_emulator& win_emu, const io_device_context& context, const uint64_t request_address,
                                const uint64_t output_address, const read_console_input_request& request)
            {
                return queue_read(win_emu, pending_read{.context = context,
                                                        .request_address = request_address,
                                                        .output_address = output_address,
                                                        .request = request});
            }

            NTSTATUS queue_read(windows_emulator& win_emu, pending_read read)
            {
                const auto context = read.context;
                if ((context.completion_port && context.apc_routine) || !context.io_status_block ||
                    (!context.event && !context.apc_routine && !context.completion_port && (!context.vcpu || !context.vcpu->active_thread)))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                auto* thread = context.vcpu ? context.vcpu->active_thread : nullptr;
                if (thread)
                {
                    read.context.issuing_thread_id = thread->id;
                }
                read.context.event = {};
                read.context.completion_port = {};
                bool queued{};
                const auto cleanup = utils::finally([&] {
                    if (!queued)
                    {
                        release_read(win_emu.process, read);
                    }
                });
                const auto issuing_thread = thread ? win_emu.process.threads.internal_handle(*thread) : handle{};
                if (!io_completion_wait::retain_reference(win_emu.process, context.event, read.context.event) ||
                    !io_completion_wait::retain_reference(win_emu.process, context.completion_port, read.context.completion_port) ||
                    !io_completion_wait::retain_reference(win_emu.process, thread, issuing_thread, read.issuing_thread))
                {
                    return STATUS_INVALID_HANDLE;
                }
                read.context.vcpu = nullptr;
                const auto text = read.text;
                pending_reads_.push_back(read);
                queued = true;
                write_io_status(context, STATUS_PENDING, true);
                if (auto* event = win_emu.process.get_object_as<sogen::event>(context.event))
                {
                    event->set_signaled(false);
                }
                if (text)
                {
                    this->work(win_emu);
                }
                const auto status = context.io_status_block.read().Status;
                if (status == STATUS_PENDING && !context.event && !context.apc_routine && !context.completion_port)
                {
                    context.thread().await_console_io = true;
                    win_emu.yield_thread(*context.vcpu);
                }
                return status;
            }

            std::vector<pending_read> pending_reads_{};
            uint16_t text_attributes_{7};
            uint32_t input_mode_{default_input_mode};
            uint32_t output_mode_{default_output_mode};
            console_coordinate cursor_position_{};
            console_font_info_response font_info_{default_console_font()};
            std::u16string title_{};
            std::u16string original_title_{};
        };
    }

    std::unique_ptr<io_device> create_console_device(const device_creation_context&)
    {
        return std::make_unique<console_device>();
    }

    NTSTATUS read_console_input(windows_emulator& win_emu, const io_device_context& context)
    {
        if (!win_emu.process.devices.get(win_emu.process.console_handle))
        {
            win_emu.process.console_handle = win_emu.process.devices.store(io_device_container{u"Console", win_emu, {}});
        }
        auto* container = win_emu.process.devices.get(win_emu.process.console_handle);
        auto* console = container->get_internal_device<console_device>();
        return console->read_input(win_emu, container->prepare_io_context(context));
    }

} // namespace sogen
