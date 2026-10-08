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
        return byte;
    }

    void buffered_console_backend::unread(const uint8_t byte)
    {
        this->input_.push_front(byte);
    }

    std::optional<console_key_event> buffered_console_backend::read_input_event()
    {
        const auto byte = this->read_byte(-1);
        if (!byte)
        {
            return std::nullopt;
        }

        if (*byte == 0x1B)
        {
            if (this->input_.empty())
            {
                this->refill(25);
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
                    this->refill(25);
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
                return console_key_event{.key = *key};
            }

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
        default:
            return console_key_event{
                .key = console_key::character,
                .character = static_cast<char16_t>(*byte),
                .control = *byte > 0 && *byte <= 0x1A,
            };
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
    }

} // namespace sogen
