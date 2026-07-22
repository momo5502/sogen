#pragma once

#include "../port.hpp"

namespace sogen
{

    std::unique_ptr<port> create_audio_service_port();

} // namespace sogen
