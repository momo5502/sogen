#pragma once

#include <cstdint>

namespace sogen::utils
{
    uint32_t current_host_process_id();

    // Whether the host process with that id is still running. State shared between emulator processes records the
    // holder's id so a survivor can recover it when its holder is gone, which a non-robust lock cannot do.
    bool host_process_alive(uint32_t process_id);
}
