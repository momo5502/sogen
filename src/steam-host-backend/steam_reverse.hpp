#pragma once

// Host reverse-callback channel: for game-implemented response interfaces (matchmaking server browser),
// the host substitutes a proxy that Steam calls into, and those calls are queued and shipped back to the
// guest. Declarations only (no SDK types) so the device/backend/generated thunks can reference them.

#include <cstdint>
#include <vector>

namespace sogen::steam_host
{
    // Creates (or reuses) a host proxy for response interface `type` (RESPONSE_IFACE_ID) bound to `token`.
    // Returned as the concrete ISteam...Response* (as void*). Called from the generated thunks.
    void* create_response_proxy(int32_t type, uint64_t token);

    // Appends queued reverse calls to `out` as reverse_record + payload; returns the record count and
    // clears the queue.
    uint32_t drain_reverse_callbacks(std::vector<unsigned char>& out);
}
