#include "../std_include.hpp"
#include "buffered_console_backend.hpp"

#if !defined(OS_WINDOWS) && !defined(OS_EMSCRIPTEN)
#include <cerrno>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace sogen
{
    namespace
    {
        class posix_console_backend final : public buffered_console_backend
        {
          public:
            ~posix_console_backend() override
            {
                this->restore_terminal();
            }

            void set_input_mode(const console_input_mode& mode) override
            {
                if (!mode.line || !mode.echo)
                {
                    this->enable_raw_terminal();
                }
                else
                {
                    this->restore_terminal();
                }
            }

            void reset() override
            {
                this->restore_terminal();
                buffered_console_backend::reset();
            }

          protected:
            void refill(const int timeout_ms) override
            {
                if (!this->input_.empty())
                {
                    return;
                }

                pollfd descriptor{STDIN_FILENO, POLLIN, 0};
                int poll_result{};
                do
                {
                    poll_result = poll(&descriptor, 1, timeout_ms);
                } while (poll_result < 0 && errno == EINTR);

                if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0)
                {
                    return;
                }

                for (;;)
                {
                    std::array<uint8_t, 256> buffer{};
                    const auto bytes_read = ::read(STDIN_FILENO, buffer.data(), buffer.size());
                    if (bytes_read > 0)
                    {
                        this->input_.insert(this->input_.end(), buffer.begin(), buffer.begin() + bytes_read);
                    }
                    else if (bytes_read < 0 && errno == EINTR)
                    {
                        continue;
                    }
                    else
                    {
                        return;
                    }

                    pollfd pending{STDIN_FILENO, POLLIN, 0};
                    do
                    {
                        poll_result = poll(&pending, 1, 0);
                    } while (poll_result < 0 && errno == EINTR);

                    if (poll_result <= 0 || (pending.revents & POLLIN) == 0)
                    {
                        return;
                    }
                }
            }

          private:
            void enable_raw_terminal()
            {
                if (terminal_raw_ || !isatty(STDIN_FILENO))
                {
                    return;
                }

                termios original{};
                if (tcgetattr(STDIN_FILENO, &original) != 0)
                {
                    return;
                }

                auto raw = original;
                raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
                raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
                raw.c_cflag &= ~(CSIZE | PARENB);
                raw.c_cflag |= CS8;
                raw.c_cc[VMIN] = 1;
                raw.c_cc[VTIME] = 0;
                if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
                {
                    original_terminal_ = original;
                    terminal_raw_ = true;
                }
            }

            void restore_terminal()
            {
                if (!terminal_raw_)
                {
                    return;
                }

                (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal_);
                terminal_raw_ = false;
            }

            termios original_terminal_{};
            bool terminal_raw_{};
        };
    }

    std::unique_ptr<console_backend> create_posix_console_backend()
    {
        return std::make_unique<posix_console_backend>();
    }

} // namespace sogen
#endif
