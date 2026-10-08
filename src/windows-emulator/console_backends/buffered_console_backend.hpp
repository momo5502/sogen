#pragma once

#include <platform/console_backend.hpp>

#include <deque>

namespace sogen
{
    class buffered_console_backend : public console_backend
    {
      public:
        bool input_available() override;
        std::optional<console_key_event> read_input_event() override;
        std::string read_input(size_t length) override;
        void reset() override;

      protected:
        virtual void refill(int timeout_ms) = 0;

        std::deque<uint8_t> input_{};

      private:
        std::optional<uint8_t> read_byte(int timeout_ms);
        void unread(uint8_t byte);
    };

} // namespace sogen
