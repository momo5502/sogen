#pragma once
#include "../port.hpp"

namespace sogen::sxs
{
    lpc_request_result handle_create_activation_context(windows_emulator& win_emu, const lpc_request_context& request);
}
