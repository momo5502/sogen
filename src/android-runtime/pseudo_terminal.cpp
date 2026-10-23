#include "std_include.hpp"
#include "pseudo_terminal.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace sogen::android
{
    namespace
    {
        void write_all(const int descriptor, std::string_view data)
        {
            while (!data.empty())
            {
                const auto written = ::write(descriptor, data.data(), data.size());
                if (written < 0 && errno == EINTR)
                {
                    continue;
                }
                if (written <= 0)
                {
                    return;
                }
                data.remove_prefix(static_cast<size_t>(written));
            }
        }

        int open_master()
        {
            const auto master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
            if (master < 0)
            {
                throw std::runtime_error("Failed to open a pseudo-terminal");
            }

            std::array<char, 128> slave_name{};
            if (grantpt(master) != 0 || unlockpt(master) != 0 || ptsname_r(master, slave_name.data(), slave_name.size()) != 0)
            {
                ::close(master);
                throw std::runtime_error("Failed to set up the pseudo-terminal");
            }

            const auto slave = ::open(slave_name.data(), O_RDWR | O_NOCTTY);
            if (slave < 0)
            {
                ::close(master);
                throw std::runtime_error("Failed to open the pseudo-terminal slave");
            }

            ::dup2(slave, STDIN_FILENO);
            ::dup2(slave, STDOUT_FILENO);
            ::close(slave);
            return master;
        }
    }

    pseudo_terminal::pseudo_terminal(output_handler handler)
        : master_(open_master()),
          handler_(std::move(handler))
    {
        std::array<int, 2> stop{};
        if (::pipe2(stop.data(), O_CLOEXEC) != 0)
        {
            ::close(this->master_);
            throw std::runtime_error("Failed to create the pseudo-terminal stop pipe");
        }

        this->stop_read_ = stop[0];
        this->stop_write_ = stop[1];
        this->reader_ = std::thread([this] { this->read_output(); });
    }

    pseudo_terminal::~pseudo_terminal()
    {
        write_all(this->stop_write_, "x");
        this->reader_.join();

        ::close(this->stop_read_);
        ::close(this->stop_write_);
        ::close(this->master_);
    }

    void pseudo_terminal::write_input(const std::string_view data) const
    {
        write_all(this->master_, data);
    }

    void pseudo_terminal::write_output(const std::string_view data)
    {
        write_all(STDOUT_FILENO, data);
    }

    void pseudo_terminal::resize(const uint16_t columns, const uint16_t rows) const
    {
        const winsize size{.ws_row = rows, .ws_col = columns, .ws_xpixel = 0, .ws_ypixel = 0};
        ::ioctl(this->master_, TIOCSWINSZ, &size);
    }

    void pseudo_terminal::read_output() const
    {
        std::array<char, 4096> buffer{};
        for (;;)
        {
            std::array descriptors{
                pollfd{.fd = this->master_, .events = POLLIN, .revents = 0},
                pollfd{.fd = this->stop_read_, .events = POLLIN, .revents = 0},
            };

            if (::poll(descriptors.data(), descriptors.size(), -1) < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return;
            }

            if (descriptors[1].revents != 0)
            {
                return;
            }

            const auto bytes_read = ::read(this->master_, buffer.data(), buffer.size());
            if (bytes_read < 0 && errno == EINTR)
            {
                continue;
            }
            if (bytes_read <= 0)
            {
                return;
            }

            this->handler_(std::string{buffer.data(), static_cast<size_t>(bytes_read)});
        }
    }
}
