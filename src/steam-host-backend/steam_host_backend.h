#pragma once

// C ABI for the Steam host backend. Deliberately free of SDK / Windows types so the emulator's device
// (which cannot include <windows.h> or the SDK headers) can call it. The implementation (a separate TU)
// includes the SDK headers, connects to the host's real steamclient64.dll, and runs the generated thunks.

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Connects to the running host Steam client. Returns 1 on success, 0 if Steam is unavailable
    // (not running / not logged in / DLL missing). Safe to call repeatedly.
    int sogen_steam_backend_init(void);

    // Resolves a versioned interface (e.g. "SteamUser023") to an opaque handle; 0 if unknown.
    uint64_t sogen_steam_backend_create_interface(const char* version);

    void sogen_steam_backend_release(uint64_t handle);

    // Invokes method `method` on `handle`. `in`/`in_len` is the marshalled argument blob (treated as
    // hostile). Out-parameter payload is written to `out` (bounded by `out_cap`, actual length in
    // `*out_len`); the raw return value goes to `*ret`. Returns a steam_host status (0 == ok).
    int sogen_steam_backend_invoke(uint64_t handle, uint32_t method, const uint8_t* in, uint32_t in_len,
                                   uint8_t* out, uint32_t out_cap, uint32_t* out_len, uint64_t* ret);

    // Drains pending callbacks from the host client. `out` receives the normal-callback records (each:
    // int32 callback_id, uint32 data_bytes, payload) followed by the reverse-callback records (each:
    // uint64 token, int32 method, uint32 data_bytes, payload). `*normal_len`/`*normal_count` and
    // `*reverse_len`/`*reverse_count` report each section. Returns 0 on success.
    int sogen_steam_backend_run_callbacks(int32_t pipe, uint8_t* out, uint32_t out_cap, uint32_t* normal_len,
                                          uint32_t* normal_count, uint32_t* reverse_len, uint32_t* reverse_count);

    // Retrieves a completed async-call result (Steam_GetAPICallResult) for CCallResult dispatch. Writes up
    // to min(data_bytes, out_cap) bytes to `out`; sets *io_failure; returns 1 if the result was available.
    int sogen_steam_backend_get_api_call_result(int32_t pipe, uint64_t call, int32_t callback_id, uint32_t data_bytes,
                                                uint8_t* out, uint32_t out_cap, uint32_t* out_len, uint8_t* io_failure);

#ifdef __cplusplus
}
#endif
