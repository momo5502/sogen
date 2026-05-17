// Scripting bridge.
//
// This is NOT a fake frontend mock: every call drives the real emulator via
// the same generic debug-command channel / debug_session used by the rest of
// the debugger. The host language is JavaScript; the injected `emu` object
// mirrors the existing nanobind `sogen` model (emu.debug.*, emu.memory.*) so a
// future CPython runtime (pyodide + nanobind-wasm) can mount behind the exact
// same facade without protocol or UI changes.

import { Emulator } from "@/emulator";
import * as dbg from "@/debugger/api";

export interface ScriptHandle {
  cancelled: boolean;
}

function buildEmuFacade(
  emulator: Emulator,
  print: (line: string) => void,
  handle: ScriptHandle,
) {
  const guard = () => {
    if (handle.cancelled) {
      throw new Error("Script cancelled");
    }
  };

  const debugApi = {
    async breakpoint(address: bigint | number) {
      guard();
      return dbg.setBreakpoint(emulator, BigInt(address));
    },
    async clear_breakpoint(address: bigint | number) {
      guard();
      return dbg.clearBreakpoint(emulator, BigInt(address));
    },
    async breakpoints() {
      guard();
      return dbg.listBreakpoints(emulator);
    },
    async step_into() {
      guard();
      return dbg.stepInto(emulator);
    },
    async step_over() {
      guard();
      return dbg.stepOver(emulator);
    },
    async step_out() {
      guard();
      return dbg.stepOut(emulator);
    },
    async run_to(address: bigint | number) {
      guard();
      return dbg.runTo(emulator, BigInt(address));
    },
    async continue_execution() {
      guard();
      return dbg.continueExecution(emulator);
    },
    // `await emu.debug.registers()` -> { rax: "0x..", rip: "0x..", ... }
    async registers() {
      guard();
      const regs = await dbg.getRegisters(emulator);
      const map: Record<string, string> = {};
      for (const r of regs) {
        map[r.name] = r.value;
      }
      return map;
    },
    async disassemble(address: bigint | number, count = 16) {
      guard();
      return dbg.disassemble(emulator, BigInt(address), count);
    },
    async modules() {
      guard();
      return dbg.getModules(emulator);
    },
    async threads() {
      guard();
      return dbg.getThreads(emulator);
    },
    async call_stack() {
      guard();
      return dbg.getCallStack(emulator);
    },
  };

  const memoryApi = {
    async read(address: bigint | number, size: number) {
      guard();
      return emulator.readMemory(BigInt(address), size);
    },
  };

  return {
    debug: debugApi,
    memory: memoryApi,
    get state() {
      return emulator.getState();
    },
  };
}

export interface ScriptResult {
  ok: boolean;
  error?: string;
}

/**
 * Run `source` as an async JavaScript function with the live `emu` facade and
 * a `print` builtin. Returns a traceback string on failure. Cancellation is
 * cooperative (checked at every emulator call).
 */
export async function runScript(
  emulator: Emulator,
  source: string,
  print: (line: string) => void,
  handle: ScriptHandle,
): Promise<ScriptResult> {
  const emu = buildEmuFacade(emulator, print, handle);

  const log = (...args: unknown[]) => {
    print(
      args
        .map((a) =>
          typeof a === "string" ? a : JSON.stringify(a, bigintReplacer),
        )
        .join(" "),
    );
  };

  try {
    const fn = new Function(
      "emu",
      "print",
      "console",
      `"use strict"; return (async () => {\n${source}\n})();`,
    );
    await fn(emu, log, { log });
    return { ok: true };
  } catch (e) {
    const err = e as Error;
    return {
      ok: false,
      error: `${err.name}: ${err.message}\n${err.stack ?? ""}`,
    };
  }
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function bigintReplacer(_key: string, value: any) {
  return typeof value === "bigint" ? "0x" + value.toString(16) : value;
}
