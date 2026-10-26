#pragma once

#include "../port.hpp"

namespace sogen
{

    std::unique_ptr<port> create_audio_service_port();

    // The ALPC port a WASAPI client connects to for a stream's endpoint commands (SetEventHandle). The audio service
    // names it in the stream's shared control data, so a connection to it identifies the stream.
    std::u16string audio_endpoint_port_name(uint32_t stream_id);
    std::optional<uint32_t> parse_audio_endpoint_port_name(std::u16string_view name);

} // namespace sogen
