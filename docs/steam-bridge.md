# Steam bridge (paravirtualized Steamworks)

The Steam bridge lets a guest Windows game running inside Sogen talk to the **real Steam client on
the host**. It is the Steam analogue of the GPU bridge: instead of re-implementing Steamworks, the
emulator forwards Steamworks interface calls out to the host's own `steamclient64.dll`, so the guest
sees the host's logged-in account, friends, stats, matchmaking, etc.

The design mirrors Proton's `lsteamclient`, but the host is Windows, so the ABI matches on both sides
and the compiler — not hand-written marshalling — handles calling conventions.

## Architecture

```
  guest game
      │  loads its own untouched steam_api(64).dll   (never modified)
      ▼
  our steamclient(64).dll  shim         src/samples/steam-shim/
      │  CreateInterface / Steam_BGetCallback / Steam_GetAPICallResult
      │  proxies derive from the real ISteam* interfaces (exact vtable)
      ▼
  \\.\SogenSteam  io_device             src/windows-emulator/devices/steam_bridge.cpp
      │  IOCTLs: get_version / create_interface / release / invoke /
      │          run_callbacks / get_api_call_result
      ▼
  steam-host-backend  (C ABI)           src/steam-host-backend/
      │  owns the SDK headers + generated thunks; loads the host steamclient64
      ▼
  host steamclient64.dll  →  real Steam client
```

**Why a `steamclient` shim, not a `steam_api` shim.** Games ship their own versioned, sometimes
DRM-wrapped `steam_api(64).dll`, and we must not touch game files. The `steamclient` boundary
(`CreateInterface` + the `Steam_*` pump) is the stable seam. The game's own `steam_api` loads our
`steamclient` and does its normal `CCallback` dispatch; we only serve the interface calls and the
callback/call-result pumps underneath it.

## Components

| Path | Role |
|------|------|
| `src/tools/steam-bridge-generator/` | Parses the SDK headers and emits guest proxies + host thunks. |
| `src/steam-generated/` | Generator output (`*.generated.hxx`). **Gitignored — Valve-derived, never committed.** Regenerate locally. |
| `src/steam-bridge-protocol/` | Wire protocol: IOCTL codes and request/response structs. Arch-agnostic (fixed 8-byte scalar slots). |
| `src/steam-host-backend/` | Static lib behind a pure-C ABI; owns SDK headers + thunks + the host `steamclient64` bring-up. |
| `src/windows-emulator/devices/steam_bridge.cpp` | The `\\.\SogenSteam` io_device. Hardened: treats all guest input as hostile. |
| `src/samples/steam-shim/` | The guest `steamclient(64).dll` shim (32- and 64-bit). |
| `src/samples/steam-shim-test/` | Standalone test that drives the steamclient boundary like a game's `steam_api` would. |

## Code generation

The generator parses the SDK's `isteam*.h` headers (not `steam_api.json`, which lists non-virtual
inline helpers and omits `STEAM_PRIVATE_API` methods, so it is not the vtable) to recover the true
ordered virtual list per interface. It emits:

- **Guest proxies** that derive from the real `ISteam*` interfaces, so the vtable layout is exact.
- **Host thunks** that call `((ISteamX*)p)->Method(...)` directly.

Both halves `#include` the real SDK headers, so the compiler emits the correct ABI (register vs.
hidden-pointer struct returns, floats in XMM, `thiscall` for 32-bit, `CSteamID` by value, …). This
is the same approach as `lsteamclient` and avoids hand-rolling ABI across ~900 methods.

Coverage is ~95% of methods (865/903). The remainder is the genuinely bespoke tail: function-pointer
params (a host callback can't cross the boundary), type-tagged `void*` configs, and a few
double-pointers — each needs per-method handling, not more generic machinery.

### Building with the SDK

Generated code is not committed. To build the bridge:

1. Point the CMake cache var `SOGEN_STEAMWORKS_SDK_DIR` at `<sdk>/public/steam`.
2. Run the generator (`generate.py --sdk <sdk>/public/steam --out-dir src/steam-generated`).
3. Configure/build. `steam-shim`, `steam-shim-test`, and the real backend are gated on the SDK var;
   without it the device falls back to a null backend and guest Steam calls fail cleanly.

## Security model

The `\\.\SogenSteam` device assumes the guest is an attacker. Every length/offset from the guest is
bounds-checked against the payload; the invoke arg blob is bounded by the input buffer and the reply
by the output buffer; guest-supplied allocation sizes are capped. Buffer marshalling is two-pass on
the host (read all self-delimiting inputs → sizes known → allocate capped output buffers → call →
write back). Unsized `char*`/`void*` pointers are never treated as single elements (that was a real
overflow) — they are stubbed unless an explicit size attribute or an adjacent size param is present.

## Callbacks

`steamclient64.dll` directly exports `Steam_BGetCallback` / `Steam_FreeLastCallback` /
`Steam_GetAPICallResult` — no `SteamAPI_Init` needed; we pump our own pipe. The host backend drains
normal callbacks and serializes them; the device forwards them on the `run_callbacks` IOCTL, keyed by
the game's pipe. Async `SteamAPICall_t` results work end-to-end via the `get_api_call_result` IOCTL.

**Reverse callbacks** (the matchmaking server browser, where Steam calls *into* game-implemented
`ISteamMatchmaking*Response` objects) are wired generically — host response proxies, a reverse-record
channel, and guest dispatch — but **delivery is unproven**: in testing Steam never invoked the host
proxy. Suspected fix is pumping the matchmaking frame host-side
(`SteamAPI_ManualDispatch_RunFrame`) and/or testing against a LAN list. Treat the server browser as
"the shape is there, nothing crashes," not "works."

## Running a real game

The game loads its **own** `steam_api` unchanged; that DLL finds our `steamclient` via the registry.
Nothing in the game folder is modified.

1. **Make the shim visible in the guest.** Either drop `steamclient(64).dll` at a guest path (e.g.
   `C:\`) or map it with `-p C:\steamclient64.dll <host-path>` and `-p C:\steamclient.dll <host-path>`.
2. **Point the registry at it.** The game's `steam_api` reads
   `HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64` (and `SteamClientDll` for 32-bit). These
   are real hive values — an overlay-only key cannot be opened by the guest, so they must be baked
   into `NTUSER.DAT`. On a Windows host: copy the registry dir, then
   `reg load` / `reg add` / `reg unload` the `ActiveProcess` block
   (`SteamClientDll`, `SteamClientDll64`, `Universe`, `ActiveUser`, `pid`) plus
   `HKCU\Software\Valve\Steam\{SteamPath,SteamExe}`, and run `analyzer -r <that-registry-dir>`.
3. **App ID.** The guest process must know its App ID or `steam_api` fails `SteamAPI_Init` with
   "Steam must be running to play this game." Pass `--env SteamAppId <id>` (and `SteamGameId`), or
   drop a `steam_appid.txt`. Separately, set the host env `SteamAppId=<id>` so the backend's own
   connection is scoped to the right app (ownership/stats/DLC); user-scoped calls work regardless.

### Faking "Steam is running"

A guest `steam_api` reads a pid from `ActiveProcess\pid`, `NtOpenProcess`'s it, and checks it is
alive. Sogen serves a synthetic Steam process so that passes:

- `handles.hpp`: `STEAM_FAKE_PROCESS_ID` (**must equal the seeded registry `pid`**) and a pseudo
  `STEAM_PROCESS_HANDLE`.
- `NtOpenProcess` returns the pseudo handle for that pid (and the guest's own handle for its own pid);
  any other pid is `STATUS_INVALID_CID`.
- The pseudo handle never signals (a liveness `WaitForSingleObject` times out = alive), and
  `NtQueryInformationProcess(ProcessBasicInformation)` reports `ExitStatus = STILL_ACTIVE`, so
  `GetExitCodeProcess` reports alive. `NtClose` already no-ops pseudo handles.

## Status

Proven in-emulator against real Steam (both 32- and 64-bit guests): interface calls return the real
account (persona, SteamID, friends), plain callbacks, and async call-results. The registry spoof and
the synthetic Steam process are verified end-to-end.

Not yet proven: reverse-callback delivery (server browser); a full retail game booting to menu.
`iw4mp.exe` (MW2), for example, reaches its own `steam_api` and our bridge, but in a headless run it
blocks earlier on a D3D9-init modal dialog before Steam is fully exercised — a graphics/bring-up
issue upstream of the bridge, not a Steam issue.
