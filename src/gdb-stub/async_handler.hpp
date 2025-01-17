#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <condition_variable>

namespace gdb_stub
{
    class async_handler
    {
      public:
        using handler = void(std::atomic_bool& should_run);
        using handler_function = std::function<handler>;

        async_handler(handler_function handler);
        ~async_handler();

        async_handler(async_handler&&) = delete;
        async_handler(const async_handler&) = delete;

        async_handler& operator=(async_handler&&) = delete;
        async_handler& operator=(const async_handler&) = delete;

        void run();
        void pause();
        bool is_running() const;

      private:
        std::atomic_bool run_{false};
        std::atomic_bool stop_{false};
        std::atomic_bool is_running_{false};

        handler_function handler_{};
        std::thread runner_{};

        void work();
    };
}
