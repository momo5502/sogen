// Scripting bridge.
//
// This is NOT a fake frontend mock: every call drives the real emulator via
// the same generic debug-command channel / debug_session used by the rest of
// the debugger. The host language is Python (CPython via Pyodide); the
// injected `emu` object mirrors the existing nanobind `sogen` model
// (emu.debug.*, emu.memory.*) so scripts read like the project's Python
// bindings. The same facade also still backs the JS runner.

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
    // Alias matching the sogen Python bindings (MemoryManager.read_memory).
    async read_memory(address: bigint | number, size: number) {
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

/* ------------------------------- Python ------------------------------- */

// Pyodide is loaded lazily from the CDN on first script run and cached for the
// session. The browser emulator has no native `sogen` wasm module, so the
// `emu` facade above (which speaks the real debug-command protocol) is what we
// expose to Python — scripts read like the nanobind bindings.

const PYODIDE_VERSION = "0.26.4";

declare global {
  interface Window {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    loadPyodide?: (config: { indexURL: string }) => Promise<any>;
  }
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
let pyodidePromise: Promise<any> | null = null;

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function loadPyodideRuntime(): Promise<any> {
  if (pyodidePromise) {
    return pyodidePromise;
  }
  pyodidePromise = new Promise((resolve, reject) => {
    const base = `https://cdn.jsdelivr.net/pyodide/v${PYODIDE_VERSION}/full/`;
    if (window.loadPyodide) {
      window.loadPyodide({ indexURL: base }).then(resolve, reject);
      return;
    }
    const script = document.createElement("script");
    script.src = base + "pyodide.js";
    script.onload = () => {
      if (!window.loadPyodide) {
        reject(new Error("Pyodide loaded but loadPyodide is missing"));
        return;
      }
      window.loadPyodide({ indexURL: base }).then(resolve, reject);
    };
    script.onerror = () =>
      reject(new Error("Failed to load the Pyodide runtime from the CDN"));
    document.head.appendChild(script);
  });
  // A failed load must not be cached forever; allow a later retry.
  pyodidePromise.catch(() => {
    pyodidePromise = null;
  });
  return pyodidePromise;
}

/**
 * Run `source` as CPython (Pyodide) with the live `emu` facade in scope and
 * Python `print` wired to the console. Emulator calls are coroutines, so
 * scripts use `await emu.debug.…`; top-level await is supported. Cancellation
 * is cooperative (checked at every emulator call).
 */
export async function runPythonScript(
  emulator: Emulator,
  source: string,
  print: (line: string) => void,
  handle: ScriptHandle,
): Promise<ScriptResult> {
  const emu = buildEmuFacade(emulator, print, handle);

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  let py: any;
  try {
    print("> loading Python runtime (first run downloads Pyodide)…");
    py = await loadPyodideRuntime();
  } catch (e) {
    const err = e as Error;
    return { ok: false, error: err.message ?? String(e) };
  }

  if (handle.cancelled) {
    return { ok: false, error: "Script cancelled" };
  }

  const emit = (text: string) => {
    // Pyodide batches stdout without a trailing newline; appendLine splits.
    print(text.replace(/\n$/, ""));
  };
  py.setStdout({ batched: emit });
  py.setStderr({ batched: emit });

  // Keep `emu` a live JsProxy (methods + awaitable promises). A deep toPy
  // conversion would flatten it into a plain dict and drop the methods.
  py.globals.set("emu", emu);
  try {
    await py.runPythonAsync(source);
    return { ok: true };
  } catch (e) {
    const err = e as Error;
    return {
      ok: false,
      error: `${err.name ?? "PythonError"}: ${err.message ?? String(e)}`,
    };
  } finally {
    py.globals.delete("emu");
  }
}
