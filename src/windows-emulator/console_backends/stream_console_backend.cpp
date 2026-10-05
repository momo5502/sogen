#include "../std_include.hpp"
#include "buffered_console_backend.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace sogen
{
    namespace
    {
        class stream_console_backend final : public buffered_console_backend
        {
          public:
            void set_input_mode(const console_input_mode& /*mode*/) override
            {
            }

          protected:
            void refill(const int timeout_ms) override
            {
                if (!this->input_.empty())
                {
                    return;
                }

                if (timeout_ms < 0)
                {
                    this->read_one();
                    return;
                }

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
                do
                {
                    if (std::cin.rdbuf()->in_avail() > 0)
                    {
                        this->read_available();
                        return;
                    }

                    if (timeout_ms == 0)
                    {
                        return;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (std::chrono::steady_clock::now() < deadline);
            }

          private:
            void read_one()
            {
                const auto character = std::cin.get();
                if (character != std::char_traits<char>::eof())
                {
                    this->input_.push_back(static_cast<uint8_t>(character));
                    this->read_available();
                }
            }

            void read_available()
            {
                while (std::cin.rdbuf()->in_avail() > 0)
                {
                    const auto character = std::cin.get();
                    if (character == std::char_traits<char>::eof())
                    {
                        return;
                    }

                    this->input_.push_back(static_cast<uint8_t>(character));
                }
            }
        };
    }

    std::unique_ptr<console_backend> create_stream_console_backend()
    {
        return std::make_unique<stream_console_backend>();
    }

} // namespace sogen
