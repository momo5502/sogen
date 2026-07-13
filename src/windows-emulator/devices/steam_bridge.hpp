#pragma once

#include "../io_device.hpp"

namespace sogen
{
    // Host endpoint of the Steam paravirtualization bridge (\\.\SogenSteam). See steam_bridge.cpp and
    // steam-bridge-protocol/steam_bridge_protocol.hpp for the wire contract.
    std::unique_ptr<io_device> create_steam_bridge(const device_creation_context& context);
}
