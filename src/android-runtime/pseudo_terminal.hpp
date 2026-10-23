#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace sogen::android
{
    // Makes the slave side of a new pseudo-terminal this process's stdin and stdout. The console backend, the
    // console size and child sandbox processes, which inherit both, then share one terminal that the app drives
    // through the master side, as a desktop terminal would.
    class pseudo_terminal
    {
      public:
        using output_handler = std::function<void(std::string)>;

        explicit pseudo_terminal(output_handler handler);
        ~pseudo_terminal();

        pseudo_terminal(const pseudo_terminal&) = delete;
        pseudo_terminal& operator=(const pseudo_terminal&) = delete;
        pseudo_terminal(pseudo_terminal&&) = delete;
        pseudo_terminal& operator=(pseudo_terminal&&) = delete;

        void write_input(std::string_view data) const;
        static void write_output(std::string_view data);
        void resize(uint16_t columns, uint16_t rows) const;

      private:
        int master_{-1};
        int stop_read_{-1};
        int stop_write_{-1};
        output_handler handler_{};
        std::thread reader_{};

        void read_output() const;
    };
}
