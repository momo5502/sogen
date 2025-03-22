#pragma once
#include <functional>

namespace utils
{
    struct interupt_handler
    {
        interupt_handler(std::function<void()> callback);
        ~interupt_handler();

        interupt_handler(interupt_handler&&) = delete;
        interupt_handler(const interupt_handler&) = delete;

        interupt_handler& operator=(interupt_handler&&) = delete;
        interupt_handler& operator=(const interupt_handler&) = delete;
    };
}
