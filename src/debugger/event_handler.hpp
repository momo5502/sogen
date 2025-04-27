#pragma once

#include <windows_emulator.hpp>

namespace debugger
{
    struct event_context
    {
        windows_emulator& win_emu;
        bool resume{false};
    };

    void handle_events(event_context& c);
}
