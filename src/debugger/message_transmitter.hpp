#pragma once

#include <chrono>
#include <string>

namespace sogen::debugger
{
    void suspend_execution(std::chrono::milliseconds ms = std::chrono::milliseconds(0));
    void send_message(const std::string& message);
    std::string receive_message();
} // namespace sogen::debugger
