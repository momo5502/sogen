#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace sogen
{
    enum class console_key
    {
        character,
        escape,
        enter,
        tab,
        backspace,
        up,
        down,
        left,
        right,
        home,
        end,
        insert,
        delete_key,
        page_up,
        page_down,
    };

    struct console_key_event
    {
        console_key key{console_key::character};
        char16_t character{};
        bool control{};
    };

    struct console_input_mode
    {
        bool processed{};
        bool line{};
        bool echo{};
    };

    class console_backend
    {
      public:
        virtual ~console_backend() = default;

        virtual bool input_available() = 0;
        virtual std::optional<console_key_event> read_input_event() = 0;
        virtual std::string read_input(size_t length) = 0;
        virtual void set_input_mode(const console_input_mode& mode) = 0;
        virtual void reset() = 0;
    };

    class null_console_backend final : public console_backend
    {
      public:
        bool input_available() override
        {
            return false;
        }

        std::optional<console_key_event> read_input_event() override
        {
            return std::nullopt;
        }

        std::string read_input(size_t /*length*/) override
        {
            return {};
        }

        void set_input_mode(const console_input_mode& /*mode*/) override
        {
        }

        void reset() override
        {
        }
    };

    std::unique_ptr<console_backend> create_default_console_backend();
    std::unique_ptr<console_backend> create_posix_console_backend();
    std::unique_ptr<console_backend> create_stream_console_backend();

} // namespace sogen
