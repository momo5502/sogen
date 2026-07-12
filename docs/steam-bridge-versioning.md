# Steam bridge — legacy interface versioning (design note)

Status: **validated prototype, not yet integrated.** Captures the plan to replace the current
legacy-version handling with a Proton/lsteamclient-style, version-exact generator built on libclang.

## The problem

Games request the interface versions they were built against (MW2 → `SteamClient008`, `SteamUser012`,
`SteamNetworking003`, …). The current bridge generates proxies only for the latest SDK and forwards old
versions onto the latest wire (prefix-forwarding for appended params, `_DEPRECATED` aliasing), **stubbing**
anything with no compatible latest twin. That loses methods: on `steamworks_sdk_105`, 60/235 methods stub —
including `InitiateGameConnection` (IWNet auth) and the whole `ISteamNetworking` socket API.

## How Proton/lsteamclient does it

One fully-generated wrapper **per interface version** (189 `.cpp` across 79 SDK snapshots). Each wrapper is
compiled against the snapshot where that version has its exact vtable, and is a thin passthrough:
`((ISteamNetworking*)p)->IsP2PPacketAvailable(pcubMsgSize, nChannel)`. It never stubs and never forwards.
Because lsteamclient bridges **Windows↔Linux**, it needs 61 `struct_converters_*.cpp` for ABI differences —
and because it's an in-process passthrough (Wine shares the address space), it does **not** need parameter
direction info at all (it forwards guest pointers straight through).

## Our approach: version-exact, libclang, no struct converters

Adopt the *technique* (not Proton's code): parse each SDK snapshot's headers with **libclang** and emit a
version-exact guest proxy **and** host thunk per version, each compiled against its own snapshot, routing
`dispatch(version)` to the matching version and calling the real version-exact object the host already
fetches via `GetISteamGenericInterface`.

Two things make this *simpler* for us than for Proton:

- Guest and host are both **Windows and the same version**, so struct layouts match byte-for-byte — **no
  struct converters** (the bulk of lsteamclient).
- We **do** need parameter directions (we marshal across the emulator IOCTL boundary, not in-process) — and
  libclang gives them for free (below).

This **deletes** the entire stub/forward/`_DEPRECATED`/prefix layer.

### Key libclang facts (validated)

- The `STEAM_*` annotations (`STEAM_OUT_STRING_COUNT`, `STEAM_ARRAY_COUNT`, `STEAM_BUFFER_COUNT`, …) expand
  to `__attribute__((annotate("out_buffer_count:cbDest;")))` **only under `-DAPI_GEN`** (see
  `steam_api_internal.h`). Parse with `-DAPI_GEN` and each appears as an `ANNOTATE_ATTR` child of the
  `PARM_DECL` — exactly the direction/size info we currently pull from `steam_api.json`.
- Parse args: `-x c++ -std=c++14 -D_WIN32 -DAPI_GEN -fms-extensions -Wno-ignored-attributes
  -target {i686,x86_64}-pc-windows-msvc -I <sdk>`, source `#include "steam_api.h"` + `steam_gameserver.h`.
- Both current and old snapshots parse with **0 errors**; vtable order = declaration order of virtual
  `CXX_METHOD`s; `STEAM_PRIVATE_API` methods show as non-`PUBLIC` `access_specifier`.
- libclang is a **generate-time only** dependency: `pip install libclang` (bundles the native lib for
  win/linux/mac). It is never a build or runtime dependency and nothing ships. Cross-target layout is a
  `-target` flag, so a Linux dev still gets Windows layouts.

### Validation (prototype front-end reusing the existing emitter/classifier)

| SDK | json+regex (current) | libclang front-end |
|---|---|---|
| current SDK | 903 methods, 865 marshalled | 903 methods, **863 marshalled** (2 edge cases to reconcile) |
| `sdk_105` | forward-to-latest: **60 stubbed** | version-exact: **4 stubbed (98%)** |

`ISteamUser012` and `ISteamNetworking003` go from several stubs each to **0** — every method MW2 needs is
marshalled directly against the 1.05 headers.

## Sourcing & licensing (the reason this is a separate decision)

The headers are Valve's **Steamworks SDK**, governed by the **Steamworks SDK License** (the `LICENSE` in
Proton's `lsteamclient` *is* that license; headers are `Copyright Valve, All rights reserved`) — **not**
Proton's GPL. That license only permits redistributing `redistributable_bin` (the DLLs), never the headers,
and clause 2.4 restricts reverse engineering. So:

- **Never** commit/vendor the headers or the generated (derived) code — gitignored, as today.
- Prefer **`FetchContent` from Valve's own `ValveSoftware/Proton`** (pinned old tag with flat
  `steamworks_sdk_*` dirs, `GIT_SHALLOW`, no submodules): the *user's* build obtains the headers directly
  from Valve; the project ships only a URL (tool-provider, not distributor).
- Gate the fetch behind the Steam opt-in so default/public builds pull nothing from Valve.
- Legacy versions are **frozen**, so an older Proton tag is a correct source for them; the current version
  comes from the official SDK.
- The open question underpinning the bridge is statutory **interoperability** rights (EU Software
  Directive Art. 6, which a no-RE clause can't waive; US §1201(f)) applied to a *paravirtualization* bridge
  that forwards to genuine Steam. We consider this defensible interoperability — the bridge reimplements,
  emulates, and circumvents nothing in Steam; it just lets a genuinely-licensed client be reached from the
  sandbox — but the precise contract-vs-statute line is jurisdiction-dependent, so it's an open legal
  question the project is tracking rather than a settled clearance.

## Remaining implementation

1. **Front-end** (prototype: `src/tools/steam-bridge-generator/clang_frontend.py`): fold into `generate.py`,
   dropping the `steam_api.json`/regex path.
2. **Version-exact emit**: emit guest proxies + host thunks per version with each version's own indices;
   route `create_proxy`/`dispatch` by version→tag; remove prefix-forwarding, `_DEPRECATED` aliasing, and
   stubbing.
3. **Build**: one isolated object library per version on **both** guest and host (mirror the existing
   `steam-shim-v105` pattern), each compiled against its snapshot.
4. **CMake**: `SOGEN_ENABLE_STEAM` option (default ON, disable-able); `FetchContent` the snapshots; run the
   generator (needs libclang).
