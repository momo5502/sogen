#pragma once
#include "../port.hpp"

namespace sogen
{
    std::unique_ptr<port> create_event_log_port();
}
