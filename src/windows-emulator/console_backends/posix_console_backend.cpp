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
                if (mode.line && mode.echo && mode.processed)
                {
                    this->restore_terminal();
                }
                else
                {
                    this->configure_terminal(mode);
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

                pollfd descriptor{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
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

                    pollfd pending{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
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
            void configure_terminal(const console_input_mode& mode)
            {
                if (!isatty(STDIN_FILENO))
                {
                    return;
                }

                if (!terminal_modified_ && tcgetattr(STDIN_FILENO, &original_terminal_) != 0)
                {
                    return;
                }

                auto configured = original_terminal_;
                if (!mode.line)
                {
                    configured.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
                    configured.c_lflag &= ~(ICANON | IEXTEN);
                    configured.c_cflag &= ~(CSIZE | PARENB);
                    configured.c_cflag |= CS8;
                    configured.c_cc[VMIN] = 1;
                    configured.c_cc[VTIME] = 0;
                }
                else
                {
                    configured.c_lflag |= ICANON;
                }

                configured.c_lflag = mode.echo ? configured.c_lflag | ECHO : configured.c_lflag & ~(ECHO | ECHONL);
                configured.c_lflag = mode.processed ? configured.c_lflag | ISIG : configured.c_lflag & ~ISIG;
                if (tcsetattr(STDIN_FILENO, TCSANOW, &configured) == 0)
                {
                    terminal_modified_ = true;
                }
            }

            void restore_terminal()
            {
                if (!terminal_modified_)
                {
                    return;
                }

                (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal_);
                terminal_modified_ = false;
            }

            termios original_terminal_{};
            bool terminal_modified_{};
        };
    }

    std::unique_ptr<console_backend> create_posix_console_backend()
    {
        return std::make_unique<posix_console_backend>();
    }

} // namespace sogen
#endif
