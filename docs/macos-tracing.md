# macOS syscall tracing

## Status and scope

`analyzer --os=macos <program>` runs an arm64 Mach-O and prints a trace in which every BSD syscall line
is followed by `--> name: value` rows carrying decoded arguments, plus a decoded errno when the call
fails. The observable gate is the ctest `macos-trace-test`.

Two things named in the design under this stage are deliberately **not** here:

- **`macos-gdb-stub`.** It needs an AArch64 GDB target description, an `arm64_gdb_stub_handler`, and a
  resume rule for `brk` (this backend leaves PC *on* the faulting instruction, so a breakpoint handler
  must advance it by 4 itself or loop forever). That is a self-contained deliverable with its own
  hazards, and folding it in here would have delayed the trace.
- **`macos-debugger`.** The Linux equivalent is the Emscripten-only event pump for the browser
  playground, with no native consumer. It belongs with the WebAssembly work that owns the browser build.

There is also no JSONL report: the Windows `--report` path is built on reflection helpers that live
inside `src/windows-analyzer/` and would have to be hoisted to shared code first.

## How a trace is produced

Two layers, and the split matters:

- **The emulator produces events.** `src/macos-emulator/macos_emulator_callbacks.hpp` declares the
  callback surface; `src/macos-emulator/trace/` decodes arguments and publishes them through
  `on_trace_detail`. All of the decoding lives here.
- **The analyzer renders them.** `src/macos-analyzer/macos_analysis.cpp` subscribes to the callbacks and
  turns them into events; `src/macos-analyzer/macos_console_reporter.cpp` turns each event into a line.

A reader who does not know this looks for the decoding in the analyzer, does not find it, and concludes
it does not exist. It is in the emulator.

## The generated tables

`syscalls.master` is **not on a stock Mac**. The SDK ships
`$(xcrun --show-sdk-path)/usr/include/sys/syscall.h` with name-to-number pairs but no argument names and
no types, which is why the tables come from Apple's published XNU source and are checked in.

Vendored inputs live in `src/tools/gen-bsd-syscall-table/vendor/` with a `provenance.json` recording the
tag and a sha256 of each file. The current tag is **xnu-12377.121.6**. `syscalls.master` carries no
licence header of its own, so the repository's top-level `APPLE_LICENSE` is vendored alongside it.

To refresh: run `fetch.sh` (network, developer machine only), then `gen_bsd_syscall_table.py`. CI
regenerates from the vendored inputs and diffs the result, so a hand-edited generated file, a generator
change that was not re-run, and a refreshed input without regeneration all fail the build.

The generator's output is checked in at
`src/macos-emulator/trace/bsd_syscall_table.generated.cpp`: a 558-entry array indexed directly by
syscall number, one entry per number `syscalls.master` assigns and a bare `{}` for the 102 numbers it
leaves unassigned. `find_bsd_syscall_prototype` turns that gap encoding into `nullptr`, which is what
lets `bsd_syscall_dispatcher` and `describe_bsd_syscall` tell "unassigned" apart from "assigned but not
implemented by sogen" instead of guessing from a missing handler alone. The dispatcher's own handler
table only knows the syscalls sogen implements, so when an unimplemented one traps, it looks the number
up in this generated table before reporting it — naming an unimplemented syscall in the
`unimplemented_syscall` diagnostic instead of printing a bare number (`bsd_syscall_dispatcher.cpp`).

Three entries are not literal transcriptions of the source, each for a stated reason:

- **The `sys_` prefix is stripped.** XNU spells 33 entries `sys_close`, `sys_fcntl`, `sys_dup` and so on
  to avoid colliding with its own kernel functions; `makesyscalls.sh` strips the prefix when it writes
  `sys/syscall.h`, and userspace only ever knows the stripped name.
- **Slot 0 is seeded as `syscall`.** The table spells it `nosys` with the comment `{ indirect syscall }`.
  The other 172 `nosys` entries really are unused slots and stay skipped.
- **Mach trap indices 3 and 4 are seeded, not parsed.** arm64 XNU dispatches `mach_absolute_time` and
  `mach_continuous_time` in `handle_svc` before the generic trap table, so they appear nowhere in
  `syscall_sw.c`.

## Argument decoding rules

`describe_bsd_syscall` classifies each argument in this order:

1. A per-syscall **override** (`open`'s flags, `mmap`'s protection, `fcntl`'s command, …).
2. A **buffer pairing**, which connects a pointer argument to the length argument that bounds it.
3. An **iovec pairing**, for `readv` and `writev`.
4. A **pointer type** — rendered as a quoted string if its name is in `path_argument_names`, otherwise
   as hex.
5. The name `fd` or `fildes`, rendered signed so `-1` reads as `-1`.
6. Otherwise signedness of the declared type.

When a syscall decodes badly, extend one of three tables in
`src/macos-emulator/trace/macos_syscall_trace.cpp`: `overrides`, `buffer_pairings`, or
`path_argument_names`. The flag tables themselves are in `trace/macos_flag_decoders.cpp` and are named
against the emulator's own constants in `macos_platform.hpp`, never against a host header — a decoder
that disagreed with those constants would describe a call the emulator did not make.

## Fail-soft behaviour

**Tracing may never terminate emulation.** The rendering of a value that cannot be read is:

| Situation | Renders as |
|---|---|
| Pointer that cannot be read | `<unreadable>` |
| Null pointer | `NULL` |
| String longer than `--string-limit` | `"…"...` |
| Non-printable buffer | hex preview, `… (N bytes)` when truncated |
| An exception anywhere in the decode | `<argument decoding failed>` |

Guest reads used by tracing are bounded and non-throwing, and go a byte at a time so a string whose tail
runs off a mapping still shows its readable prefix.

## Mach traps

XNU records `MACH_TRAP(name, arg_count, munge_count, munger)` and nothing else — there are no argument
names and no types anywhere for these entries. So a Mach trap traces as its name plus `arg0..argN-1` in
hex, never more rows than the trap declares. Richer decoding is hand-placed in the Mach handlers through
`on_mach_port` and `on_mach_message`.

## The command line

`--os={windows,linux,macos}` selects the personality. Precedence: an explicit `--os`, then the magic
bytes of the first file named on the command line, then `EMULATOR_LINUX=1`, then Windows. The environment
variable still works because the browser playground's worker sets it.

macOS flags: `-e/--emulation/--root`, `-s/--silent`, `-v/--verbose`, `-c/--concise`, `--skip-syscalls`,
`--skip-args`, `--call-count`, `-i/--ignore`, `--string-limit`, `--buffer-limit`, `--max-instructions`,
`--env`.

Two ordering notes that cost real time if unknown:

- **Flags must precede the executable.** Everything after it is passed to the guest, so
  `--max-instructions` written last is a guest argument and the run is unbounded.
- **With `-e`, the executable is a guest path**, resolved through the root.

## Known gaps

- These callbacks are declared and rendered but have no call sites yet, so nothing currently emits them:
  `on_dyld_image`, `on_thread_set_name`, `on_mach_port`, `on_mach_message`, `on_generic_activity`,
  `on_suspicious_activity`, `on_memory_violate` and `on_module_unload`. They exist so the handlers that
  will report into them have somewhere to report rather than needing to be retrofitted.
- An unprototyped syscall number prints positional hex rows for its non-zero arguments only.
- No JSONL report and no GDB stub, as above.
