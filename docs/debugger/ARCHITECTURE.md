# Integrated Debugger — Architecture

Status: Phase 1 (core abstraction + design). Implemented incrementally; this
document is the contract the later phases build against.

## Goal

A first-class, in-browser debugger that operates against the **real** emulator
execution engine (Unicorn/WHP/icicle/KVM via `windows_emulator`), not a
frontend-only mock. IDA/x64dbg/Ghidra-style UX: disassembly, registers, stack,
memory, breakpoints, threads, call stack, modules, CFG, and a Python console.

## Reuse map (do not reinvent)

| Concern | Existing system reused |
|---|---|
| Single-step | `windows_emulator::start(1)` (see `x86_64_gdb_stub_handler::singlestep`) |
| Breakpoints | `x86_64_emulator::hook_memory_execution(addr, cb)` + RAII `scoped_hook` (see `x86_64_gdb_stub_handler::set_breakpoint`) |
| Disassembly | `disassembler` (Capstone; Intel/AT&T handles; `resolve_jump_target`) |
| Registers | `windows_emulator::emu().read_register/write_register`, `x86_register` |
| Memory | `memory_manager::try_read_memory`, `get_reserved_regions` (already surfaced via `GetMemoryRegions`) |
| Modules | `module_manager::modules()`, `find_name`, `mapped_module` |
| Threads / PC | `process_context::threads`, `active_thread`, `current_thread().current_ip` |
| Transport | FlatBuffers event channel `events.fbs` ↔ `event_handler.cpp` |
| Event pump (browser) | `analysis.cpp` calls `debugger::handle_events` every 0x20000 insns |
| Client bridge | `page/src/emulator.ts` worker + id/tombstone request correlation |
| Scripting model | existing nanobind `sogen` Python API (`src/python-bindings`) |

## Components

```
                 ┌──────────────────────── browser ────────────────────────┐
 React debugger  │  Disasm | Regs | Stack | Mem | BP | Threads | CallStack  │
   UI panels     │  Modules | CFG (React Flow) | Python (Monaco)            │
        │        └───────────────┬──────────────────────────────────────────┘
        │  typed wrappers        │ Emulator.debugCommand(kind, json) -> Promise
        ▼                        ▼
  page/src/debugger/*  ──>  emulator.ts  ──(postMessage)──>  emulator-worker.js
                                                                  │ FlatBuffers
                                                                  ▼
                       event_handler.cpp  ──dispatch──>  debug_session (core)
                                                                  │
                              windows_emulator (emu/memory/modules/threads)
```

- **`debug_session` (core, `src/debugger/debug_session.hpp`)** — the first-class
  subsystem. Owns breakpoint registry, step engine, and introspection. Wraps a
  `windows_emulator&`. Backend-agnostic: only uses `emu()` interfaces that all
  emulator backends implement, so Unicorn/WHP/icicle/KVM behave identically.
- **Generic command protocol** — instead of a bespoke FlatBuffers table per
  feature, one extensible pair:
  - `DebugCommandRequest { id:uint32; kind:uint32; payload:[ubyte] }`
  - `DebugCommandResponse { id:uint32; ok:bool; payload:[ubyte] }`
  `payload` is UTF-8 JSON (same proven approach as `GetMemoryRegionsResponse`).
  New debugger features = new `kind` + JSON schema; **no FlatBuffers regen**.
- **`event_handler.cpp` dispatch** — decodes `DebugCommandRequest`, routes
  `kind` to a `debug_session` method, serializes the JSON result into
  `DebugCommandResponse` with the same `id`.

## Command kinds (JSON sub-protocol)

```
0  get_registers      -> { rip, registers:{name:value(hex)} }
1  disassemble {address,count} -> { insns:[{address,bytes,mnemonic,op,symbol,branch}] }
2  get_modules        -> { modules:[{name,base,size,entry}] }
3  get_threads        -> { threads:[{id,ip,active}] }
4  get_callstack {thread?} -> { frames:[{ip,sp,symbol,module}] }
5  set_breakpoint {address,type} -> { id }
6  clear_breakpoint {address}    -> { ok }
7  list_breakpoints   -> { breakpoints:[{address,type,enabled}] }
8  step_into          -> async stop event
9  step_over          -> async stop event
10 step_out           -> async stop event
11 run_to {address}    -> async stop event
12 continue / pause / restart
13 read_memory {address,size}    (already covered by ReadMemoryRequest)
```

Async stops reuse the existing pause/state machinery (`GetStateResponse`,
`EmulationStatus`) so the UI updates are event-driven and low-latency.

## Event flow

1. UI action → `Emulator.debugCommand(kind, json)` → unique `id`, pending map
   (tombstone FIFO, identical to `readMemory` correlation).
2. Worker forwards FlatBuffers `DebugCommandRequest`.
3. `handle_events` (pumped from the emulation loop) drains it while paused;
   `event_handler` dispatches to `debug_session`.
4. Response `DebugCommandResponse{id,...}` → worker → `emulator.ts` resolves the
   promise by `id`.
5. Execution-state changes (hit breakpoint, step complete) flip the existing
   `emulation_state` → existing `GetStateResponse` path → React re-render.

## Stepping implementation (Phase 3 contract)

- **step_into**: `emu().start(1)` — one instruction, then pause. Proven path
  (`gdb_stub_handler::singlestep`). Thread-aware: step applies to
  `current_thread()`.
- **step_over**: decode the instruction at RIP via `disassembler`. If it is a
  `call` (or `rep`-prefixed), set a **temporary** execute hook at
  `RIP + insn.size`, `continue`, auto-remove on hit. Otherwise == step_into.
- **step_out**: read return address from the stack frame
  (`[RSP]` after prologue / via call-stack walk), temp breakpoint there,
  `continue`.
- **run_to**: temporary breakpoint at target, `continue`.
- **No desync rule**: stepping never mutates persistent emulator state; temp
  hooks are RAII (`scoped_hook`) and removed before the pause is reported.
  All step decisions are made from a paused snapshot.

## Breakpoint lifecycle

```
set_breakpoint(addr)
  └─> debug_session stores breakpoint{addr,type,enabled}
      └─> scoped_hook = emu().hook_memory_execution(addr, on_hit)
on_hit (during emulation):
  └─> mark session paused -> emulation loop's handle_events blocks
      └─> emit state change -> UI shows stop @ addr
clear_breakpoint(addr) -> erase entry -> scoped_hook dtor removes emu hook
```

Breakpoints are kept in a `utils::concurrency::container` (same as the gdb
stub) for async-safe access between the emulation thread and command handlers.

## State synchronization

- Single source of truth = the live `windows_emulator`. The UI never caches
  authoritative state; every panel reads via commands against the current
  paused snapshot.
- A monotonic `snapshot generation` (already used by the memory view) is
  bumped on each fresh pause so panels/caches invalidate consistently.
- All introspection is **read-only** and only valid while paused; commands
  issued while running are rejected (`ok:false`) — never block emulation.

## Future extension points

`debug_session` is the seam for: taint tracking (hook-driven), trace recording
(instruction hook ring buffer), time-travel (periodic
`memory_manager` snapshot + replay), symbolic execution (decode stream feeds an
expression engine). None require protocol changes — only new command `kind`s.

## Python scripting integration

The browser Python tab does not fork the API: it drives the **same**
`debug_session` via command `kind`s, exposing an `emu.debug.*` facade whose
names/behavior mirror the existing nanobind `sogen` model
(`emu.debug.breakpoint/step_into/step_over/continue_execution`,
`emu.debug.registers.rip`, `emu.debug.disassemble(addr,n)`). Execution sandbox,
output streaming, tracebacks, and cancellation are a dedicated phase; the
substrate (generic command channel + `debug_session`) is designed so the Python
layer is a thin client, not a parallel engine.

## Phase plan

1. **[done]** Core abstraction + this document.
2. **[done]** Generic `DebugCommand` protocol + dispatch + read-only
   introspection (registers/disassemble/modules/threads/callstack).
3. **[done]** Breakpoint + step engine (into/over/out/run-to),
   event-driven stops via the persistent control hook.
4. **[done]** Web UI debugger shell + panels + hotkeys
   (F5/F10/F11/Shift+F11/Ctrl+G).
5. **[done]** CFG view (self-contained SVG, incremental from decoded
   blocks).
6. **[done]** Monaco editor scripting tab + `emu.debug.*` bridge to the
   real `debug_session`.

## Scripting tab — honest scope

The scripting tab uses the Monaco editor and exposes an `emu` object
whose method names/behaviour mirror the existing nanobind `sogen`
model (`emu.debug.breakpoint/step_into/step_over/continue_execution`,
`emu.debug.registers()`, `emu.debug.disassemble(addr, n)`,
`emu.memory.read`). Every call drives the **real** emulator through the
same generic command channel / `debug_session` — it is not a mock.

The host language is JavaScript (async), not CPython: running the real
nanobind `sogen` module in the browser needs a Python-in-WASM runtime
(pyodide + nanobind-wasm). That runtime is a future drop-in that mounts
behind the *identical* `emu` facade — no protocol, `debug_session`, or
UI changes required. This keeps the integration real and correct today
instead of shipping a fake Python interpreter.
