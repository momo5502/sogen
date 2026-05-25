#pragma once

#include <linux_emulator.hpp>

namespace sogen::linux_debugger
{
    enum class emulation_state
    {
        none,
        running,
        paused,
    };

    struct event_context
    {
        linux_emulator& linux_emu;
        emulation_state state{emulation_state::none};
    };

    void handle_events(event_context& c);
    void pause_before_start(event_context& c);
    void handle_exit(const linux_emulator& linux_emu, std::optional<int> exit_status);
    void update_emulation_status(const linux_emulator& linux_emu);
} // namespace sogen::linux_debugger
