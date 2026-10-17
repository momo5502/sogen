#pragma once

#include <process_manager.hpp>

namespace sogen
{
    class windows_emulator;

    void install_inherited_pipes(windows_emulator& emulator, const process_create_request& request,
                                 std::vector<process_inherited_pipe> pipes);
}
