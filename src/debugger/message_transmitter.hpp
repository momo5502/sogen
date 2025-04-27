#pragma once

#include <nlohmann/json.hpp>

namespace debugger
{
    void suspend_execution(const std::chrono::milliseconds ms = 0ms);
    void send_object(const nlohmann::json& json);
    nlohmann::json receive_object();
}
