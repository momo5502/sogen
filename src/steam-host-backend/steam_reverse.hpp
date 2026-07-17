#pragma once

// Host reverse-callback channel: for game-implemented response interfaces (matchmaking server browser),
// the host substitutes a proxy that Steam calls into, and those calls are queued and shipped back to the
// guest. Declarations only (no SDK types) so the device/backend/generated thunks can reference them.

#include <cstdint>
#include <vector>

namespace sogen::steam_host
{
    // Creates (or reuses) a host proxy for response interface `type` (steam_bridge::response_type) bound to `token`.
    // Returned as the concrete ISteam...Response* (as void*). Called from the generated thunks.
    void* create_response_proxy(int32_t type, uint64_t token);

    // Opaque void* handles (e.g. HServerListRequest) that a Steam method returned. The guest never sees the
    // real steamclient pointer -- it gets a dense index instead (register_opaque_handle, on the returning
    // thunk), and a guest-supplied index is translated back to the real handle (resolve_opaque_handle, on the
    // consuming thunk), returning 0 for any index the host never issued. Called from the generated thunks.
    uint64_t register_opaque_handle(uint64_t real_handle); // -> dense opaque index handed to the guest
    uint64_t resolve_opaque_handle(uint64_t opaque_index); // -> real host handle, 0 if never issued

    // Appends queued reverse calls to `out` as reverse_record + payload; returns the record count and
    // clears the queue.
    uint32_t drain_reverse_callbacks(std::vector<unsigned char>& out);
}
