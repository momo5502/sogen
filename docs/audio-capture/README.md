# WASAPI audio — host-capture tooling

Reference material for finishing the emulated Windows Audio Service (`src/windows-emulator/ports/audio_service.cpp`).
The full design notes live in the assistant memory file `project_audio_wasapi.md`.

DirectSound layers on this same audio service (plus the registry endpoint database, the svcctl SCM port, and
window/thread queries); its bring-up is documented in [`../directsound.md`](../directsound.md).

## State

Working (committed): MMDevice enumeration (`GetDefaultAudioEndpoint`), `IAudioClient::GetMixFormat`, and
`OpenStream` (the `{D574D111}` "AudioClientRpc" interface, opnums 0/4). The WASAPI walk in
`src/samples/audio-sample` reaches `IAudioClient::Initialize`, which calls `CreateRemoteStream` (opnum 7) and
returns a 1232-byte `SYSTEM_AUDIO_STREAM`.

Remaining for `Initialize` to succeed (and the sample to print `AUDIO OK`):

1. Deliver the render-section handle via an **ALPC HANDLE attribute** (the emulator's
   `NtAlpcSendWaitReceivePort` currently ignores message attributes). Layout reversed below.
2. Create a guest-mappable render section (emulator `section` + backing).
3. Produce the **NDR64** inline wire for the nested `SYSTEM_AUDIO_STREAM`.

## Files

- `system_audio_stream_inmemory.bin` — the 1232-byte `SYSTEM_AUDIO_STREAM` as it sits *in memory* after
  `CAudioClient::CreateRemoteStream` on a real host (so it contains client-side pointers/vtables, not the wire
  form). Captured via `capture-inmemory-struct.cdb`. Useful for the scalar field values
  (nAvgBytesPerSec=0x5dc00=384000, nBlockAlign=8, a session GUID, buffer dur 0x989680, etc.).
- `capture-inmemory-struct.cdb` — cdb script: break at `CAudioClient::CreateRemoteStream`, step out, dump the
  `[out]` arg.
- `capture-alpc-reply.cdb` — cdb script (WIP): capture the raw ALPC reply message + receive attributes.
- `disasm-alpc-getmessageattr.cdb` / `disasm-alpc-headersize.cdb` — disassemble the ntdll ALPC attribute
  helpers (the attribute buffer layout).

## ALPC attribute layout (reversed from ntdll x64)

`AlpcGetMessageAttribute(buf, flag)` = `buf + 8 + Σ sizes of allocated attrs with bits higher than flag`
(attributes laid out highest-bit-first after the 8-byte `{AllocatedAttributes; ValidAttributes;}` header).
Per-attribute sizes: SECURITY `0x20`, VIEW `0x20`, CONTEXT `0x18`, HANDLE `0x18`, TOKEN `0x08`, DIRECT `0x08`.
The audio reply allocates CONTEXT|HANDLE, so the HANDLE attribute lands at `attrs + 8 + 0x18 = +0x20`.

## How to run a capture

The WASAPI sample runs on a normal host too:

```
build\release\artifacts\audio-sample.exe        # prints AUDIO OK on the host
cdb.exe -cf docs\audio-capture\capture-inmemory-struct.cdb build\release\artifacts\audio-sample.exe
```

Symbols must load before deferred breakpoints resolve — the scripts use `sxe ld:audioses; g; .reload /f`.
