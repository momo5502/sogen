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

### Interface versions are not unique across snapshots

An interface version string does **not** identify a single vtable layout. Valve appended methods to an
interface without bumping its version, so the same string means different things in different SDK
snapshots. `SteamFriends005`, for example:

| snapshot | methods |
|----------|---------|
| 1.02x, 1.03 | 22 |
| 1.04–1.08 | 23 (`ActivateGameOverlayToStore` appended at index 22) |
| 1.09 | 24 (`SetPlayedWith` appended at index 23) |

So a tag lookup cannot just take the first snapshot that claims a version — that can hand the game a
vtable *shorter* than the one its `steam_api` was built against, and a call to a late slot runs off the
end of the vtable into whatever follows it in `.rdata`. (MW2 called `ActivateGameOverlayToStore` and
landed on the next class's `vtable[-1]`, i.e. its RTTI Complete Object Locator, then executed RTTI data
as code.)

The changes are only ever *appends*, so the longest variant is a superset that satisfies every game:
**both sides pick the snapshot with the most methods for the requested version** (`sogen_make_proxy` in
the shim, the dispatch in the backend). They must use the same rule, or a method index would mean
different things on each side. Each tag exports `sogen_steam_method_count_<tag>` for this, backed by the
per-tag `steam_versions.generated.hxx` table.

That "append-only" property is load-bearing, so it was checked against all snapshots rather than
assumed. 34 version strings are vended with differing layouts; in every one the shorter layouts are
prefixes of the longest. No snapshot ever reorders or inserts a method under a fixed version string —
the only same-index differences are deprecation *renames*, which keep the slot and signature
(`SteamGameServer010` slot 12 `SetGameType`→`SetGameTags`; `SteamUser021` slots 3/4
`InitiateGameConnection`→`..._DEPRECATED`; `SteamClient017` slots 31/32 `Set_SteamAPI_…`→`DEPRECATED_…`).
If a future snapshot ever *did* reorder under a fixed version, picking the longest would silently
mis-dispatch, so re-run that check when adding snapshots.

### Building the bridge

Generated code is not committed. Building is gated behind the `SOGEN_ENABLE_STEAM` CMake option (ON by
default):

1. CMake `FetchContent`s the Steamworks SDK header snapshots vendored in Valve's `ValveSoftware/Proton`
   repo (tag `proton_7.0`), so the headers come straight from Valve and are never stored in this tree.
2. At configure time `generate.py` (which needs the `libclang` pip package) parses each snapshot and
   emits version-exact proxies + host thunks into the build dir.
3. If Python or `libclang` is missing, or the fetch yields no snapshots, the bridge disables itself with
   a warning and the device falls back to a null backend (guest Steam calls fail cleanly).

## Security model

The guest is untrusted (it may be malware). The bridge forwards to the host's **real** Steam client,
which runs *outside* the sandbox with the host user's account and filesystem — so there are two distinct
attack surfaces: memory corruption in the marshalling, and Steam methods that are unsafe by *semantics*.

### Memory safety of the wire path

The `\\.\SogenSteam` device bounds-checks every length/offset from the guest: the invoke arg blob is
bounded by the input buffer and the reply by the output buffer; guest-supplied allocation sizes are
capped (`cap_bytes`/`cap_count`, 16 MiB). Buffer marshalling is two-pass on the host (read all
self-delimiting inputs → sizes known → allocate capped buffers → call → write back). Unsized
`char*`/`void*` pointers are never treated as single elements — they are stubbed unless an explicit
size attribute or an adjacent size param is present.

> **Buffer counts are clamped to the allocation, not just the allocation to the cap.** A buffer's size
> param is a separate by-value arg that also travels to the real Steam method. Capping only the
> allocation while passing the raw count would let a guest count `> 16 MiB` make the method read/write
> past the (smaller) host buffer — a host-heap OOB and the one real corruption bug found in audit. The
> generator therefore clamps the count handed to Steam down to the allocated capacity, so declared size
> and buffer size can never diverge. (`generate.py`, `clamp_count`.)

### API-level policy (default-forward, with a blocklist)

Marshalling correctness is not enough: some Steamworks methods are escape/exfil primitives regardless of
how safely they are marshalled. The generator refuses to forward a blocklist of them — the host thunk
emits `blocked(...); return steam_host_unsupported;` (fail closed; the guest sees an unimplemented
method). See `BLOCKED_INTERFACES` / `BLOCKED_METHODS` in `generate.py`. The blocklist focuses on:

- **Host filesystem** — `ISteamUGC` (host-path read → public upload), `ISteamScreenshots` (host-path
  read), `ISteamHTMLSurface` (`file://`, JS, host clipboard), `ISteamRemoteStorage::UGCDownloadToLocation`
  (writes an arbitrary host path), `ISteamApps::{GetFileDetails,GetAppInstallDir,MarkContentCorrupt,
  InstallDLC,UninstallDLC,Request*ProofOfPurchaseKey*}`, `ISteamInput::SetInputActionManifestFilePath`.
  Plain `ISteamRemoteStorage` file I/O (`FileRead`/`FileWrite`/streams/`Delete`) is **not** blocked — it
  is Steam Cloud, an isolated per-app virtual filesystem the guest cannot escape to reach arbitrary host
  files; blocking it only broke legitimate cloud saves.
- **Credential minting / account mutation** — `ISteamUser` auth tickets
  (`GetAuthSessionTicket`/`GetAuthTicketForWebApi`/`RequestStoreAuthURL`/`*EncryptedAppTicket`/
  `AdvertiseGame`), `ISteamGameServer::GetAuthSessionTicket`, `ISteamFriends` overlay-web /
  protocol-registration / persona-rename / social-send methods, and `ISteamRemoteStorage`
  `FileShare`/`Publish*` (post a file publicly under the host account).

Network reach (`ISteamHTTP`, the networking/sockets interfaces) is deliberately **not** blocked: the
guest already has host network access through the emulated AFD socket device, so blocking it here buys
nothing. Identity reads and `ISteamFriends::ActivateGameOverlayToStore` (the DLC/store button) are
intentionally allowed. Matching is by method *name* per snapshot, so a version that lacks a listed
method simply never matches, and the list survives the version-drift the bridge already handles.

Reverse-callback (`callback_token`) params also reject a guest `type` outside the known range instead of
handing a null proxy to Steam (which would crash the host on the next virtual dispatch).

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
3. **App ID.** Two *independent* App ID contexts must both be correct:
   - **Guest side** — the guest process must know its App ID or `steam_api` fails `SteamAPI_Init`
     with "Steam must be running to play this game." Pass `--env SteamAppId <id>` (and `SteamGameId`),
     or drop a `steam_appid.txt`. Many retail games (e.g. `iw4mp.exe`) also bake the id in internally.
   - **Host side** — set the host env `SteamAppId=<id>` (on the `analyzer.exe`/`sandbox.exe` process)
     so the backend's `steamclient64` session is scoped to the right app. `connect_locked()` reads it
     via `std::getenv` at first use and, if unset, **falls back to 480 (Spacewar) with a loud stderr
     warning** so the SDK test harness still works.

   > **This host-side scoping is not optional for multiplayer.** Steam scopes lobbies, matchmaking,
   > rich presence and P2P to the running app. A session in the wrong app (e.g. the 480 fallback) can
   > `CreateLobby` and auto-enter its *own* lobby (`LobbyEnter_t response=1`), but real Steam rejects
   > joining *another app's* lobby with `k_EChatRoomEnterResponseDoesntExist (2)`. In MW2 this
   > surfaced as an instant `EXE_HOSTUNREACH` public-match kick: the failed `JoinLobby` left the game
   > alone in a solo lobby (`GetNumLobbyMembers==1`), so the CL connect code fell back to an empty
   > default lobby-id, `sscanf` parsed nothing, and the connect bailed. Setting host `SteamAppId` to
   > the game's real id (MW2 Multiplayer = **10190**) fixes it and the account shows as in-game in the
   > friends list. So "user-scoped calls work regardless" holds only for identity/friends reads —
   > **lobby/matchmaking joins require the correct host app context.**

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

**MW2 (`iw4mp.exe`, 32-bit WoW64) boots to menu, goes online, and joins public matches.** With the
correct host `SteamAppId=10190` (see "App ID" above) the full Steam matchmaking path works:
`RequestLobbyList`/`CreateLobby`/`JoinLobby`, lobby data/member reads, `LobbyEnter_t`/`LobbyCreated_t`
callbacks, and `ISteamUser::InitiateGameConnection` for the IWNet Steam-auth handshake. The earlier
"blocks on a D3D9-init modal dialog" note is obsolete (that dialog is dismissed by seeding the game's
first-run state; unrelated to the bridge).

## WIP / known gaps

- **Reverse-callback delivery (server browser) — unproven.** Steam calling *into* game-implemented
  `ISteamMatchmaking*Response` objects is wired generically (host response proxies + reverse-record
  channel + guest dispatch) but never observed firing in testing. Suspected fix: pump the matchmaking
  frame host-side (`SteamAPI_ManualDispatch_RunFrame`) and/or test against a LAN list. Treat as "the
  shape is there, nothing crashes," not "works." MW2's match path does not depend on it (it uses
  lobbies + direct UDP), so this is only a gap for games that drive the server browser.
- **Method coverage ~95% (865/903).** The bespoke tail (function-pointer params, type-tagged `void*`
  configs, a few double-pointers) is stubbed and needs per-method handling.
- **Advertised local address is loopback.** The client advertises `addrLoc=127.0.0.1` in lobby data
  (`get_local_address`→`getsockname` returns loopback under the emulator). Harmless for the paths
  exercised so far, but a real defect for any peer that would dial the local address.
- **Generated code is never committed** (Valve-derived; see "Building the bridge"). Only the
  hand-written bridge is tracked; regenerate `src/steam-generated/` locally.
- **Distribution rests on an open interoperability question** (Steamworks SDK license vs. EU Software
  Directive Art. 6 / US §1201(f)) — tracked in `steam-bridge-versioning.md`, not a settled clearance.
