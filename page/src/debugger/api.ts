// Typed client over the generic debugger command channel
// (see docs/debugger/ARCHITECTURE.md). Every call is read-only/paused-only
// and resolves parsed JSON, or null when unsupported/not paused.

import { Emulator, DebugCommandKind } from "@/emulator";

export interface RegisterValue {
  name: string;
  value: string; // hex, e.g. "0x140001000"
  size: number;
}

export interface Instruction {
  address: string; // hex
  mnemonic: string;
  operands: string;
  symbol: string;
  size: number;
  isCall: boolean;
  isJump: boolean;
  isReturn: boolean;
  branch?: string; // hex target, if resolved
}

export interface ModuleInfo {
  name: string;
  base: string;
  size: number;
  entry: string;
}

export interface ThreadInfo {
  id: number;
  ip: string;
  active: boolean;
}

export interface StackFrame {
  ip: string;
  sp: string;
  module: string;
}

export interface BreakpointInfo {
  address: string;
  type: number;
  enabled: boolean;
}

async function call(
  emulator: Emulator,
  kind: DebugCommandKind,
  args: Uint8Array = new Uint8Array(0),
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
): Promise<any | null> {
  const result = await emulator.debugCommand(kind, args);
  if (!result || !result.ok) {
    return null;
  }
  return result.data;
}

export async function getRegisters(
  emulator: Emulator,
): Promise<RegisterValue[]> {
  const data = await call(emulator, DebugCommandKind.GetRegisters);
  return data?.registers ?? [];
}

export async function disassemble(
  emulator: Emulator,
  address: bigint,
  count: number,
): Promise<Instruction[]> {
  const args = Emulator.encodeDebugArgs([
    ["u64", address],
    ["u32", count],
  ]);
  const data = await call(emulator, DebugCommandKind.Disassemble, args);
  return data?.instructions ?? [];
}

export async function getModules(emulator: Emulator): Promise<ModuleInfo[]> {
  const data = await call(emulator, DebugCommandKind.GetModules);
  return data?.modules ?? [];
}

export async function getThreads(emulator: Emulator): Promise<ThreadInfo[]> {
  const data = await call(emulator, DebugCommandKind.GetThreads);
  return data?.threads ?? [];
}

export async function getCallStack(emulator: Emulator): Promise<StackFrame[]> {
  const data = await call(emulator, DebugCommandKind.GetCallStack);
  return data?.frames ?? [];
}

export async function listBreakpoints(
  emulator: Emulator,
): Promise<BreakpointInfo[]> {
  const data = await call(emulator, DebugCommandKind.ListBreakpoints);
  return data?.breakpoints ?? [];
}

export async function setBreakpoint(
  emulator: Emulator,
  address: bigint,
): Promise<BreakpointInfo[]> {
  const args = Emulator.encodeDebugArgs([
    ["u64", address],
    ["u8", 0],
  ]);
  const data = await call(emulator, DebugCommandKind.SetBreakpoint, args);
  return data?.breakpoints ?? [];
}

export async function clearBreakpoint(
  emulator: Emulator,
  address: bigint,
): Promise<BreakpointInfo[]> {
  const args = Emulator.encodeDebugArgs([["u64", address]]);
  const data = await call(emulator, DebugCommandKind.ClearBreakpoint, args);
  return data?.breakpoints ?? [];
}

export function stepInto(emulator: Emulator): Promise<unknown> {
  return call(emulator, DebugCommandKind.StepInto);
}

export function stepOver(emulator: Emulator): Promise<unknown> {
  return call(emulator, DebugCommandKind.StepOver);
}

export function stepOut(emulator: Emulator): Promise<unknown> {
  return call(emulator, DebugCommandKind.StepOut);
}

export function runTo(emulator: Emulator, address: bigint): Promise<unknown> {
  const args = Emulator.encodeDebugArgs([["u64", address]]);
  return call(emulator, DebugCommandKind.RunTo, args);
}

export function continueExecution(emulator: Emulator): Promise<unknown> {
  return call(emulator, DebugCommandKind.Continue);
}
