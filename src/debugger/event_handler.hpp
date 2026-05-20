#pragma once

#include <windows_emulator.hpp>

namespace sogen::debugger
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
        bool in_break_loop{false};
    };

    void handle_events(event_context& c);
    void handle_exit(const windows_emulator& win_emu, std::optional<NTSTATUS> exit_status);
    void reset_debug_session() noexcept;
    void update_emulation_status(const windows_emulator& win_emu);

    enum class step_request
    {
        none,
        cont,
        into,
        over,
        step_out,
    };

    void enter_breakpoint(windows_emulator& win_emu, uint64_t address);
    void request_resume(step_request request, uint64_t run_to_address = 0);
    bool step_should_break(uint64_t address);
} // namespace sogen::debugger
