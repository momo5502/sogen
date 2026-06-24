# DirectSound bring-up

How `DirectSoundCreate8` and the DirectSound buffer lifecycle were brought up on the emulator, what each fix
does, and where the path currently stops. The reproduction sample is `src/samples/dsound-sample`
(`CoInitializeEx → DirectSoundCreate8 → SetCooperativeLevel → CreateSoundBuffer → Lock/Unlock → Play/Stop`).

This builds on the WASAPI work in [`audio-capture/README.md`](audio-capture/README.md) and the
`{D574D111}` "AudioClientRpc" IDL in [`audio-capture/audioclient_rpc_idl.txt`](audio-capture/audioclient_rpc_idl.txt).

## State

| Step                  | Result      |
| --------------------- | ----------- |
| `CoInitializeEx`      | `S_OK`      |
| `DirectSoundCreate8`  | `S_OK`      |
| `SetCooperativeLevel` | `S_OK`      |
| `CreateSoundBuffer`   | `S_OK`      |
| `Lock` / `Unlock`     | `S_OK`      |
| `Play`                | **fails**   |

`Play` fails at the very end of the render-start path (`0x88880302`). Everything up to and including the audio
client activation (`GetMixFormat / IsFormatSupported / GetDevicePeriod / Initialize / CreateStream`) succeeds.
The remaining blocker is described in [Remaining blocker](#remaining-blocker-createstream-format-mismatch).

WASAPI (`src/samples/audio-sample`, `AUDIO OK`) is unaffected by all of this.

## Layering

DirectSound on Vista+ is a thin front end over the audio engine:

```
dsound.dll  ──►  mmdevapi.dll (IMMDeviceEnumerator, IAudioClient activation)
            ──►  audioses.dll (IAudioClient/AudioClientRpc client stubs)
            ──►  RPC over ALPC  ──►  emulated Audiosrv  (src/windows-emulator/ports/audio_service.cpp)
```

Unlike WASAPI, DirectSound also leans on subsystems the WASAPI sample never touches:

* **MMDevice *enumeration*** (`EnumAudioEndpoints`) — DirectSound enumerates render endpoints to build its
  device list; WASAPI only resolves the default endpoint id via `GetDefaultAudioEndpoint` (RPC opnum 25).
* **The Service Control Manager** (svcctl) — mmdevapi watches the `AudioSrv` service while creating a render
  client.
* **Window/thread queries** — `GetWindowThreadProcessId(GetDesktopWindow())` for the buffer's focus thread.

Each of these needed a fix.

## Fixes

### 1. Expose the RDP endpoint as a *local* render device

Files: `src/windows-emulator/registry/registry_manager.cpp` (`alias_remote_audio_endpoints`).

A headless/RDP host has no physical audio device: the local `MMDevices\Audio\Render` key is empty and the only
endpoint lives under `RemoteRender`. Two problems:

1. `RemoteRender` was only *per-key* path-mapped into `Render` (so `Render\{id}` opened, but **enumerating**
   `Render\` returned nothing). DirectSound (and the legacy waveOut path) enumerate `Render\` subkeys, so they
   saw zero devices → `DSERR_NODRIVER`.
2. mmdevapi's `EnumAudioEndpoints` reads each device's **`Protocol` DWORD** and only exposes a device whose
   `Protocol` is `0`/absent. RDP endpoints set a non-zero `Protocol` to mark themselves remote, so even when
   enumerable they were filtered out. (Decompiled from `mmdevapi!FUN_180007880`: includes a device only when
   `size == 4 && protocol == 0`.)

Fix: when the local folder is empty, **alias the whole folder** `Render → RemoteRender` (enumerable, not just
openable) and **overlay `Protocol = 0`** on each RDP endpoint (overrides the hive value for by-name reads).
Also overlay `DeviceState = ACTIVE`. The redirected endpoint now enumerates as a local render device.

### 2. svcctl (Service Control Manager) RPC port

Files: `src/windows-emulator/ports/service_control.{hpp,cpp}`, wired in `src/windows-emulator/port.cpp`
(replacing the old no-op `\RPC Control\ntsvcs` port).

While creating a render client, mmdevapi opens the `AudioSrv` service over the **svcctl** interface
`{367ABB81-9844-35F1-AD32-98F038001003}`: `OpenSCManagerW → OpenServiceW → SubscribeServiceChangeNotifications`.
The old `ntsvcs` port returned empty replies, so the client got `RPC_X_BAD_STUB_DATA` unmarshalling the
expected context handle.

Gotchas reversed on the wire:

* svcctl uses **classic NDR (4-byte referent ids)**, *not* NDR64 like the audio interface. Tell by the
  `ROpenSCManagerW` request: `00 00 02 00 | 0f 00 00 00 | 00 00 00 00 | 0f 00 00 00 | "ServicesActive"`.
* This Windows build's opnums are shifted: `ROpenSCManagerW` arrives as **opnum 64** (its `[in]` is the
  `"ServicesActive"` database string + access, no handle), `ROpenServiceW` = 16, `RCloseServiceHandle` = 0.
* `SubscribeServiceChangeNotifications` → `sechost!I_ScQueryServiceConfig` (**opnum 55**). Its `[out]` is a
  **non-encapsulated union discriminated by the change-mask**; the selected arm is a unique pointer
  (`FC_UP → FC_SIMPLE_POINTER → FC_HYPER`) to an 8-byte WNF state name. The wire that finally unmarshalled:
  *discriminant (echo the in-mask), then the arm pointer referent, then the return value, then the deferred
  8-byte hyper*. We return `ERROR_ACCESS_DENIED`, which `SubscribeServiceChangeNotifications` passes straight
  back without touching WNF, and mmdevapi treats as non-fatal.

The port returns valid classic-NDR context handles for the open/close calls and an empty success for other
interfaces on the port (PnP / `CM_*`). With this, `DirectSoundCreate8` returns `S_OK`.

### 3. `GetWindowThreadProcessId` / desktop-window owner

Files: `src/windows-emulator/user_handle_table.hpp` (`set_owner`),
`src/windows-emulator/process_context.cpp` (`create_thread`), `src/windows-emulator/syscalls/user.cpp`
(`NtUserCreateWindowEx`).

`SetCooperativeLevel(GetDesktopWindow(), …)` does **not** store the hwnd — dsound calls
`GetWindowThreadProcessId(hwnd)` and stores the **owning thread id**, which `Play` later rejects if it is `0`
(`DSERR_PRIOLEVELNEEDED`, "Cooperative level must be set before calling Play").

user32's **client-side** `GetWindowThreadProcessId` reads the owning thread from the shared handle entry's
`pOwner` field (`USER_HANDLEENTRY + 0x8`) and returns `0` outright when it is unset — and the emulator's
`allocate_object` never populated it, so the API returned `0` for *every* window. (`GetDesktopWindow()` itself
returns the right handle; it's the *thread* lookup that failed. Note user32 reads `pOwner` directly, not via
the `NtUserQueryWindow` syscall.)

Fix: record the owning thread id in the handle entry when a window is created (`NtUserCreateWindowEx`), and —
because the desktop window is created during process setup before any thread exists — attribute it to the
initial thread in `process_context::create_thread`.

### 4. AudioClient activation opnums (`{D574D111}`)

Files: `src/windows-emulator/ports/audio_service.cpp`.

DirectSound's `Play` drives the full `IAudioClient` activation, which probes more opnums than WASAPI:

| opnum | method                         | WASAPI | DirectSound | emulator |
| ----- | ------------------------------ | ------ | ----------- | -------- |
| 0     | `GetMixFormat`                 | ✓      | ✓           | existing |
| 1     | `IsFormatSupported`            |        | ✓           | **added** → `S_OK`, null closest match |
| 2     | `GetDevicePeriod`              |        | ✓           | **added** → 10 ms default / 3 ms min |
| 4     | `Initialize`                   | ✓      | ✓           | existing |
| 7     | `CreateStream`                 | ✓      | ✓           | existing |
| 8/9   | `StartStream` / `StopStream`   | ✓      |             | existing |
| 13    | `DestroyStream`                |        | ✓ (teardown)| **added** → `S_OK` |

The unhandled opnums made audioses map the RPC failure to `AUDCLNT_E_DEVICE_INVALIDATED`
(`audioses!MapRPCResultToAudioClient`, the default for any unrecognized RPC error). With 1/2/13 handled,
DirectSound completes the activation, `Initialize` and `CreateStream`.

## Remaining blocker: CreateStream format mismatch

After `CreateStream` (opnum 7), DirectSound **destroys** the stream (opnum 13) + **disconnects** (opnum 5)
instead of starting it (no opnum 8). Tracing the raw error: audioses raises `RPC_X_BAD_STUB_DATA`
(`0x800706f7`) unmarshalling the CreateStream reply, which `MapRPCResultToAudioClient` turns into `E_INVALIDARG`
(`0x80070057`); dsound then maps that to the `Play` failure `0x88880302`.

* Opnum 4 (`Initialize`) **succeeds** — DirectSound reaches opnum 7 after it, and the opnum-4 request size
  (392) and reply (32 bytes: null `[out]` string + context handle + `S_OK`) are identical for DirectSound and
  WASAPI. (An earlier call chain that appeared to point at `Initialize` was stale-stack noise — the bad-stub
  site `audioses+0x613F6` is in the NDR runtime.)
* The emulator's opnum-7 reply (`handle_create_stream`) is a **captured 120-byte `SYSTEM_AUDIO_STREAM` wire**
  carrying WASAPI's mix format (44.1 kHz / 32-bit float, `nAvgBytesPerSec = 0x56220`). DirectSound asks for
  48 kHz / 16-bit. The reply that WASAPI's stub accepts, DirectSound's stub rejects.

Next idea: make `GetMixFormat`/`CreateStream` report DirectSound's requested format (48 kHz / 16-bit) so the
stream wire is self-consistent, or capture a real `CreateStream` reply for a 16-bit PCM stream. This is a
format-marshalling effort; since the emulator can't render audio to hardware, `Play` succeeding would be the
only payoff. DirectSound otherwise works through its entire buffer lifecycle.

## Debugging / reproduction

The investigation used a temporary, env-gated per-instruction hook in
`windows_emulator::on_instruction_execution` (gated by `EMULATOR_DSOUND_TRACE`, removed from the tree). The
patterns that were useful:

* **Find where an HRESULT is raised**: on each instruction, if `(rax & 0xffffffff) == <hresult>`, log the
  module+rva of `address` and walk the stack (`[rsp + i*8]`) for return addresses that resolve to a module.
  Catch the *raw* RPC error (`RPC_X_BAD_STUB_DATA 0x800706f7`) rather than the mapped `E_INVALIDARG` — the
  latter is also produced (benignly) during `DirectSoundCreate8`, masking the Play one. Gate on a flag set when
  `dsound + 0x24d40` (`IDirectSoundBuffer::Play`) is entered.
* Some HRESULTs are loaded as immediates and never sit in `rax` at an instruction boundary (e.g.
  `DSERR_PRIOLEVELNEEDED 0x88780046`) — for those, find the `MOV reg, 0x...` sites via Ghidra
  (`search_instructions`) and hook those rvas instead.
* `EMULATOR_LOG_RPC=1` logs every RPC call (`[audiosrv] call iface=… opnum=…`) and every reply
  (`[rpc] opnum=… reply ndr(N) handles(H): …`), and dumps the request bytes of unhandled opnums. This is how
  the opnum sequence and reply layouts were nailed.
* `EMULATOR_WHP=1` speeds up emulation but **changes the audio path behaviour** (the per-instruction hook is
  also unavailable) — use the interpreter for this work.

### Ghidra (headless MCP)

The interfaces were reversed with the Ghidra headless MCP against `dsound.dll`, `mmdevapi.dll`, `sechost.dll`,
`user32.dll` and `audioses.dll`. Tip: their tracelog/format helpers are frequently mis-flagged *noreturn*,
truncating decompiles — disable the analyzer **"Non-Returning Functions - Discovered"** *before* the first
analysis (or clear the flow-overrides and re-analyze) to avoid it.

## Commits

* `expose RDP endpoint as a local device + add svcctl SCM port`
* `finish svcctl notify marshalling + fix GetWindowThreadProcessId`
* `handle AudioClient IsFormatSupported / GetDevicePeriod / DestroyStream`
