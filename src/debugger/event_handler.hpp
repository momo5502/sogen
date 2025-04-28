#pragma once

#include <windows_emulator.hpp>

namespace debugger
{
    enum class emulation_state
    {
        none,
        running,
        paused,
    };

    struct event_context
    {
        windows_emulator& win_emu;
        emulation_state state{emulation_state::none};
    };

    void handle_events(event_context& c);
}
