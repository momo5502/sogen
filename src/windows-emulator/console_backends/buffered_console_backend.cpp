#include "../std_include.hpp"
#include "buffered_console_backend.hpp"

namespace sogen
{
    namespace
    {
        std::optional<console_key> decode_escape_sequence(const char prefix, const std::string_view sequence)
        {
            if (sequence.empty() || (prefix == 'O' && sequence.size() != 1))
            {
                return std::nullopt;
            }

            switch (sequence.back())
            {
            case 'A':
                return console_key::up;
            case 'B':
                return console_key::down;
            case 'C':
                return console_key::right;
            case 'D':
                return console_key::left;
            case 'H':
                return console_key::home;
            case 'F':
                return console_key::end;
            case '~':
                if (sequence == "1~" || sequence == "7~")
                {
                    return console_key::home;
                }
                if (sequence == "4~" || sequence == "8~")
                {
                    return console_key::end;
                }
                if (sequence == "2~")
                {
                    return console_key::insert;
                }
                if (sequence == "3~")
                {
                    return console_key::delete_key;
                }
                if (sequence == "5~")
                {
                    return console_key::page_up;
                }
                if (sequence == "6~")
                {
                    return console_key::page_down;
                }
                break;
            default:
                break;
            }

            return std::nullopt;
        }
    }

    bool buffered_console_backend::input_available()
    {
        if (this->input_.empty())
        {
            this->refill(0);
        }

        return !this->input_.empty();
    }

    std::optional<uint8_t> buffered_console_backend::read_byte(const int timeout_ms)
    {
        if (this->input_.empty())
        {
            this->refill(timeout_ms);
        }

        if (this->input_.empty())
        {
            return std::nullopt;
        }

        const auto byte = this->input_.front();
        this->input_.pop_front();
        if (this->read_recording_)
        {
            this->read_recording_->push_back(byte);
        }
        return byte;
    }

    void buffered_console_backend::unread(const uint8_t byte)
    {
        this->input_.push_front(byte);
        if (this->read_recording_ && !this->read_recording_->empty() && this->read_recording_->back() == byte)
        {
            this->read_recording_->pop_back();
        }
    }

    std::optional<console_key_event> buffered_console_backend::read_input_event(const bool wait, const bool remove)
    {
        if (remove)
        {
            return this->read_input_event_impl(wait);
        }

        std::vector<uint8_t> consumed;
        const auto pending_events = this->pending_events_;
        this->read_recording_ = &consumed;
        const auto event = this->read_input_event_impl(wait);
        this->read_recording_ = nullptr;
        while (!consumed.empty())
        {
            this->input_.push_front(consumed.back());
            consumed.pop_back();
        }
        this->pending_events_ = pending_events;
        return event;
    }

    std::optional<console_key_event> buffered_console_backend::read_input_event_impl(const bool wait)
    {
        if (!this->pending_events_.empty())
        {
            const auto event = this->pending_events_.front();
            this->pending_events_.pop_front();
            return event;
        }

        const auto byte = this->read_byte(wait ? -1 : 0);
        if (!byte)
        {
            return std::nullopt;
        }

        if (*byte == 0x1B)
        {
            if (this->input_.empty())
            {
                this->refill(wait ? 25 : 0);
            }
            if (this->input_.empty())
            {
                return console_key_event{.key = console_key::escape};
            }

            const auto prefix = this->read_byte(0);
            if (!prefix || (*prefix != '[' && *prefix != 'O'))
            {
                if (prefix)
                {
                    this->unread(*prefix);
                }
                return console_key_event{.key = console_key::escape};
            }

            std::array<char, 16> sequence{};
            size_t sequence_size{};
            while (sequence_size < sequence.size())
            {
                if (this->input_.empty())
                {
                    this->refill(wait ? 25 : 0);
                }
                const auto next = this->read_byte(0);
                if (!next)
                {
                    break;
                }

                sequence[sequence_size++] = static_cast<char>(*next);
                const auto final = sequence[sequence_size - 1];
                if (*prefix == 'O' || (final >= 0x40 && final <= 0x7E))
                {
                    break;
                }
            }

            if (const auto key = decode_escape_sequence(static_cast<char>(*prefix), std::string_view{sequence.data(), sequence_size}))
            {
                const bool control = sequence_size >= 3 && sequence[sequence_size - 3] == ';' && sequence[sequence_size - 2] == '5';
                return console_key_event{.key = *key, .control = control};
            }

            for (size_t i = sequence_size; i > 0; --i)
            {
                this->unread(static_cast<uint8_t>(sequence[i - 1]));
            }
            this->unread(*prefix);
            return console_key_event{.key = console_key::escape};
        }

        switch (*byte)
        {
        case '\n':
        case '\r':
            return console_key_event{.key = console_key::enter, .character = u'\r'};
        case '\t':
            return console_key_event{.key = console_key::tab, .character = u'\t'};
        case '\b':
        case 0x7F:
            return console_key_event{.key = console_key::backspace, .character = u'\b'};
        default: {
            if (*byte >= 0x80)
            {
                uint32_t code_point{};
                size_t continuation_count{};
                uint32_t minimum{};
                if (*byte >= 0xC2 && *byte <= 0xDF)
                {
                    code_point = *byte & 0x1F;
                    continuation_count = 1;
                    minimum = 0x80;
                }
                else if (*byte >= 0xE0 && *byte <= 0xEF)
                {
                    code_point = *byte & 0x0F;
                    continuation_count = 2;
                    minimum = 0x800;
                }
                else if (*byte >= 0xF0 && *byte <= 0xF4)
                {
                    code_point = *byte & 0x07;
                    continuation_count = 3;
                    minimum = 0x10000;
                }
                else
                {
                    return console_key_event{.key = console_key::character, .character = u'\uFFFD'};
                }

                std::array<uint8_t, 4> sequence{*byte};
                size_t sequence_size = 1;
                for (size_t i = 0; i < continuation_count; ++i)
                {
                    const auto next = this->read_byte(wait ? -1 : 0);
                    if (!next)
                    {
                        for (size_t j = sequence_size; j > 0; --j)
                        {
                            this->unread(sequence[j - 1]);
                        }
                        return std::nullopt;
                    }
                    if ((*next & 0xC0) != 0x80)
                    {
                        this->unread(*next);
                        return console_key_event{.key = console_key::character, .character = u'\uFFFD'};
                    }
                    sequence[sequence_size++] = *next;
                    code_point = (code_point << 6) | (*next & 0x3F);
                }

                if (code_point < minimum || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
                {
                    return console_key_event{.key = console_key::character, .character = u'\uFFFD'};
                }
                if (code_point > 0xFFFF)
                {
                    code_point -= 0x10000;
                    this->pending_events_.push_back(console_key_event{.key = console_key::character,
                                                                      .character = static_cast<char16_t>(0xDC00 + (code_point & 0x3FF))});
                    return console_key_event{.key = console_key::character,
                                             .character = static_cast<char16_t>(0xD800 + (code_point >> 10))};
                }
                return console_key_event{.key = console_key::character, .character = static_cast<char16_t>(code_point)};
            }

            return console_key_event{
                .key = console_key::character,
                .character = static_cast<char16_t>(*byte),
                .control = *byte > 0 && *byte <= 0x1A,
            };
        }
        }
    }

    std::string buffered_console_backend::read_input(const size_t length)
    {
        std::string data{};
        if (length == 0)
        {
            return data;
        }

        data.reserve(length);
        if (const auto byte = this->read_byte(-1))
        {
            data.push_back(static_cast<char>(*byte));
        }
        else
        {
            return data;
        }

        while (data.size() < length)
        {
            const auto byte = this->read_byte(0);
            if (!byte)
            {
                break;
            }

            data.push_back(static_cast<char>(*byte));
        }

        return data;
    }

    void buffered_console_backend::reset()
    {
        this->input_.clear();
        this->pending_events_.clear();
    }

} // namespace sogen
