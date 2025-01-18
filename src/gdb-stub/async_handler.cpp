#include "async_handler.hpp"
#include <utils/finally.hpp>

using namespace std::chrono_literals;

namespace gdb_stub
{
    async_handler::async_handler(handler_function h)
        : handler_(std::move(h))
    {
        this->stop_ = false;
        this->runner_ = std::thread([this] {
            this->work(); //
        });
    }

    async_handler::~async_handler()
    {
        this->stop_ = true;
        this->run_ = false;

        if (this->runner_.joinable())
        {
            this->runner_.join();
        }
    }

    void async_handler::pause()
    {
        this->run_ = false;

        while (this->is_running_ && !this->stop_)
        {
            std::this_thread::sleep_for(1ms);
        }
    }

    void async_handler::run()
    {
        if (this->stop_)
        {
            return;
        }

        this->run_ = true;

        while (!this->is_running_ && !this->stop_)
        {
            std::this_thread::sleep_for(1ms);
        }
    }

    bool async_handler::is_running() const
    {
        return this->is_running_;
    }

    void async_handler::work()
    {
        while (true)
        {
            while (!this->run_ && !this->stop_)
            {
                this->is_running_ = false;
                std::this_thread::sleep_for(10ms);
            }

            if (this->stop_)
            {
                break;
            }

            const auto _ = utils::finally([this] {
                this->is_running_ = false; //
            });

            this->is_running_ = true;
            this->handler_(this->run_);
        }
    }
}
