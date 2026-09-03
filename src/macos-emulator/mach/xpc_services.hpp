#pragma once

#include "mach_msg.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sogen
{
    class macos_emulator;
}

namespace sogen::xpc
{
    // Measured 2026-08-27 on the host: launchd's lookup result and every daemon's reply on an XPC
    // connection carry this msgh_id, NOT the request's. The check-in routines on the bootstrap port are
    // the exception -- those echo the request id.
    constexpr int32_t XPC_REPLY_ID = 0x20000000;

    enum class xpc_service_decision
    {
        refuse_connection_invalid,
        answer,

        // The lookup produces sogen's own window-server root port rather than a stand-in: the routines
        // sent to it are answered by gui/macos_window_server_mig.cpp, which is a real server and not a
        // refusal.
        window_server,
    };

    // Nullopt for a name whose lookup must fail outright; every other name gets a live service port
    // whose messages are refused one by one, except the names sogen actually implements.
    std::optional<xpc_service_decision> decide_xpc_service(std::string_view name);

    // What a refused service answers every message with.
    std::vector<uint8_t> refused_service_reply_body();

    // The refusal an NSXPC invocation gets: the reply block run with every argument at its zero value.
    // Nullopt when the message is not one, or when its reply signature has a type with no encoding here.
    std::optional<std::vector<uint8_t>> nsxpc_refusal_body(macos_emulator& emu, std::span<const uint8_t> body);

    // The reply to a message sent to a service port sogen stands in for. Header included.
    std::vector<uint8_t> answer_service_message(macos_emulator& emu, const mach::msg_call& call, std::span<const uint8_t> body);
}
