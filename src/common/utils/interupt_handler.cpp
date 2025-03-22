#include "interupt_handler.hpp"
#include <atomic>
#include <stdexcept>
#include <thread>

#include "win.hpp"

#ifndef _WIN32
#include <csignal>
#endif

namespace utils
{
    namespace
    {
        struct signal_data
        {
            std::atomic_bool installed{false};
            std::function<void()> handler{};
        };

        signal_data& get_signal_data()
        {
            static signal_data data{};
            return data;
        }

#ifdef _WIN32
        BOOL WINAPI handler(const DWORD signal)
        {
            const auto& data = get_signal_data();

            if (signal == CTRL_C_EVENT && data.handler)
            {
                data.handler();
            }

            return TRUE;
        }

#else
        void handler(int signal)
        {
            const auto& data = get_signal_data();

            if (signal == SIGINT && data.handler)
            {
                data.handler();
            }
        }
#endif
    }

    interupt_handler::interupt_handler(std::function<void()> callback)
    {
        auto& data = get_signal_data();

        bool value{false};
        if (!data.installed.compare_exchange_strong(value, true))
        {
            throw std::runtime_error("Global signal handler already installed");
        }

        data.handler = std::move(callback);

#ifdef _WIN32
        SetConsoleCtrlHandler(handler, TRUE);
#else
        signal(SIGINT, handler);
#endif
    }

    interupt_handler::~interupt_handler()
    {
#ifdef _WIN32
        SetConsoleCtrlHandler(handler, FALSE);
#else
        signal(SIGINT, SIG_DFL);
#endif

        std::this_thread::yield();

        auto& data = get_signal_data();

        data.handler = {};
        data.installed = false;
    }
}
