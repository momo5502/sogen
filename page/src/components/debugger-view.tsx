import React from "react";

import { List, type RowComponentProps } from "react-window";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Tabs, TabsList, TabsTrigger, TabsContent } from "@/components/ui/tabs";
import { cn } from "@/lib/utils";

import {
  PlayFill,
  PauseFill,
  XLg,
  ArrowDownShort,
  ArrowBarDown,
  ArrowUpShort,
  CursorFill,
} from "react-bootstrap-icons";

import { Emulator } from "@/emulator";
import { HexViewer } from "@/components/memory-view";
import { CfgView } from "@/components/cfg-view";
import { ScriptConsole } from "@/components/script-console";
import * as dbg from "@/debugger/api";

const ROW_HEIGHT = 20;
const DISASM_COUNT = 256;

function parseAddress(value: string): bigint | null {
  const t = value
    .trim()
    .replace(/^0x/i, "")
    .replace(/[`_\s]/g, "");
  if (!t || !/^[0-9a-fA-F]+$/.test(t)) {
    return null;
  }
  try {
    return BigInt("0x" + t);
  } catch {
    return null;
  }
}

const fmt = (a: bigint) => "0x" + a.toString(16).padStart(12, "0");

/* ----------------------------- disassembly ----------------------------- */

interface DisasmRowProps {
  insns: dbg.Instruction[];
  rip: bigint;
  breakpoints: Set<string>;
  onToggleBreakpoint: (address: bigint) => void;
  onFollow: (address: bigint) => void;
}

function DisasmRow({
  index,
  style,
  insns,
  rip,
  breakpoints,
  onToggleBreakpoint,
  onFollow,
}: RowComponentProps<DisasmRowProps>) {
  const insn = insns[index];
  if (!insn) {
    return <div style={style} />;
  }
  const address = BigInt(insn.address);
  const isCurrent = address === rip;
  const hasBp = breakpoints.has(address.toString(16));

  return (
    <div
      style={style}
      className={cn(
        "flex items-center gap-2 px-1 font-mono text-xs whitespace-pre",
        isCurrent && "bg-primary/20",
      )}
    >
      <button
        title="Toggle breakpoint"
        onClick={() => onToggleBreakpoint(address)}
        className={cn(
          "h-3 w-3 shrink-0 rounded-full border border-muted-foreground/40",
          hasBp && "border-red-500 bg-red-500",
        )}
      />
      <span
        className={cn(
          "w-28 shrink-0",
          isCurrent ? "text-primary" : "text-muted-foreground",
        )}
      >
        {isCurrent ? "▶ " : "  "}
        {fmt(address).slice(2)}
      </span>
      <span className="w-16 shrink-0 font-semibold">{insn.mnemonic}</span>
      <span className="flex-1 truncate">
        {insn.branch ? (
          <button
            className="text-sky-400 hover:underline"
            onClick={() => onFollow(BigInt(insn.branch!))}
          >
            {insn.operands}
          </button>
        ) : (
          insn.operands
        )}
        {insn.symbol && (
          <span className="ml-2 text-muted-foreground">; {insn.symbol}</span>
        )}
      </span>
    </div>
  );
}

/* ------------------------------- main view ------------------------------ */

export interface DebuggerViewProps {
  emulator: Emulator;
  paused: boolean;
  onClose: () => void;
}

export function DebuggerView({ emulator, paused, onClose }: DebuggerViewProps) {
  const [width, setWidth] = React.useState(() =>
    Math.min(900, Math.round(window.innerWidth * 0.7)),
  );
  const dragging = React.useRef(false);

  const [generation, setGeneration] = React.useState(0);
  const wasPaused = React.useRef(false);

  const [registers, setRegisters] = React.useState<dbg.RegisterValue[]>([]);
  const [insns, setInsns] = React.useState<dbg.Instruction[]>([]);
  const [modules, setModules] = React.useState<dbg.ModuleInfo[]>([]);
  const [threads, setThreads] = React.useState<dbg.ThreadInfo[]>([]);
  const [frames, setFrames] = React.useState<dbg.StackFrame[]>([]);
  const [breakpoints, setBreakpoints] = React.useState<dbg.BreakpointInfo[]>(
    [],
  );
  const [rip, setRip] = React.useState<bigint>(BigInt(0));
  const [rsp, setRsp] = React.useState<bigint>(BigInt(0));
  const [selected, setSelected] = React.useState<bigint | null>(null);
  const [tab, setTab] = React.useState("disasm");
  const addressInput = React.useRef<HTMLInputElement>(null);

  const bpSet = React.useMemo(
    () => new Set(breakpoints.map((b) => BigInt(b.address).toString(16))),
    [breakpoints],
  );

  // Resizable docked width.
  React.useEffect(() => {
    const move = (e: MouseEvent) => {
      if (!dragging.current) return;
      const next = window.innerWidth - e.clientX;
      setWidth(Math.min(Math.max(next, 480), window.innerWidth - 160));
    };
    const up = () => {
      if (dragging.current) {
        dragging.current = false;
        document.body.style.userSelect = "";
      }
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
    return () => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
    };
  }, []);

  // Each fresh pause is a new snapshot: refetch everything.
  React.useEffect(() => {
    if (paused && !wasPaused.current) {
      setGeneration((g) => g + 1);
    }
    wasPaused.current = paused;
  }, [paused]);

  const refresh = React.useCallback(async () => {
    const regs = await dbg.getRegisters(emulator);
    setRegisters(regs);
    const ripReg = regs.find((r) => r.name === "rip");
    const rspReg = regs.find((r) => r.name === "rsp");
    const ripVal = ripReg ? BigInt(ripReg.value) : BigInt(0);
    const rspVal = rspReg ? BigInt(rspReg.value) : BigInt(0);
    setRip(ripVal);
    setRsp(rspVal);
    setSelected(ripVal);
    setInsns(await dbg.disassemble(emulator, ripVal, DISASM_COUNT));
    setModules(await dbg.getModules(emulator));
    setThreads(await dbg.getThreads(emulator));
    setFrames(await dbg.getCallStack(emulator));
    setBreakpoints(await dbg.listBreakpoints(emulator));
  }, [emulator]);

  React.useEffect(() => {
    if (!paused) {
      return;
    }
    // refresh() only setStates after awaits (never synchronously in the
    // effect body), so the cascading-render concern does not apply.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    refresh();
  }, [generation, paused, refresh]);

  const followAddress = React.useCallback(
    async (address: bigint) => {
      setSelected(address);
      setTab("disasm");
      setInsns(await dbg.disassemble(emulator, address, DISASM_COUNT));
    },
    [emulator],
  );

  const toggleBreakpoint = React.useCallback(
    async (address: bigint) => {
      const key = address.toString(16);
      const updated = bpSet.has(key)
        ? await dbg.clearBreakpoint(emulator, address)
        : await dbg.setBreakpoint(emulator, address);
      setBreakpoints(updated);
    },
    [emulator, bpSet],
  );

  const doStep = React.useCallback(
    async (fn: (e: Emulator) => Promise<unknown>) => {
      if (!paused) return;
      await fn(emulator);
      emulator.updateState(); // backend stops again -> new pause -> refresh
    },
    [emulator, paused],
  );

  const run = React.useCallback(() => {
    if (paused) {
      emulator.resume();
    }
  }, [emulator, paused]);

  // Hotkeys: F5 run, F10 over, F11 into, Shift+F11 out, Ctrl+G goto.
  React.useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "F5") {
        e.preventDefault();
        run();
      } else if (e.key === "F10") {
        e.preventDefault();
        doStep(dbg.stepOver);
      } else if (e.key === "F11" && !e.shiftKey) {
        e.preventDefault();
        doStep(dbg.stepInto);
      } else if (e.key === "F11" && e.shiftKey) {
        e.preventDefault();
        doStep(dbg.stepOut);
      } else if (e.key.toLowerCase() === "g" && e.ctrlKey) {
        e.preventDefault();
        addressInput.current?.focus();
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [run, doStep]);

  const submitAddress = () => {
    const addr = parseAddress(addressInput.current?.value ?? "");
    if (addr !== null) {
      followAddress(addr);
    }
  };

  return (
    <div
      className="relative flex h-full shrink-0 flex-col border-l bg-background"
      style={{ width: `${width}px`, maxWidth: "100vw" }}
    >
      <div
        onMouseDown={(e) => {
          e.preventDefault();
          dragging.current = true;
          document.body.style.userSelect = "none";
        }}
        title="Drag to resize"
        className="absolute inset-y-0 left-0 z-20 w-1.5 cursor-col-resize hover:bg-primary/40"
      />

      <div className="flex flex-wrap items-center gap-1 border-b p-2">
        <span className="mr-2 text-sm font-medium">Debugger</span>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Run (F5)"
          disabled={!paused}
          onClick={run}
        >
          <PlayFill />
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Pause"
          disabled={paused}
          onClick={() => emulator.pause()}
        >
          <PauseFill />
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Step Into (F11)"
          disabled={!paused}
          onClick={() => doStep(dbg.stepInto)}
        >
          <ArrowDownShort />
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Step Over (F10)"
          disabled={!paused}
          onClick={() => doStep(dbg.stepOver)}
        >
          <ArrowBarDown />
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Step Out (Shift+F11)"
          disabled={!paused}
          onClick={() => doStep(dbg.stepOut)}
        >
          <ArrowUpShort />
        </Button>
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Run To Cursor"
          disabled={!paused || selected === null}
          onClick={() =>
            selected !== null && doStep((e) => dbg.runTo(e, selected))
          }
        >
          <CursorFill />
        </Button>
        <form
          className="flex items-center"
          onSubmit={(e) => {
            e.preventDefault();
            submitAddress();
          }}
        >
          <Input
            ref={addressInput}
            placeholder="Go to address (Ctrl+G)"
            spellCheck={false}
            className="h-7 w-44 font-mono text-xs"
            disabled={!paused}
          />
        </form>
        <div className="flex-1" />
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-7"
          title="Close"
          onClick={onClose}
        >
          <XLg />
        </Button>
      </div>

      <Tabs
        value={tab}
        onValueChange={setTab}
        className="flex flex-1 flex-col min-h-0"
      >
        <TabsList className="m-1 flex-wrap">
          <TabsTrigger value="disasm">Disassembly</TabsTrigger>
          <TabsTrigger value="registers">Registers</TabsTrigger>
          <TabsTrigger value="stack">Call Stack</TabsTrigger>
          <TabsTrigger value="breakpoints">Breakpoints</TabsTrigger>
          <TabsTrigger value="threads">Threads</TabsTrigger>
          <TabsTrigger value="modules">Modules</TabsTrigger>
          <TabsTrigger value="cfg">CFG</TabsTrigger>
          <TabsTrigger value="memory">Memory</TabsTrigger>
          <TabsTrigger value="script">Script</TabsTrigger>
        </TabsList>

        <div className="relative flex flex-1 min-h-0">
          <TabsContent value="disasm" className="absolute inset-0">
            {insns.length > 0 ? (
              <List
                rowCount={insns.length}
                rowHeight={ROW_HEIGHT}
                className="h-full"
                style={{ height: "100%" }}
                rowComponent={DisasmRow}
                rowProps={{
                  insns,
                  rip,
                  breakpoints: bpSet,
                  onToggleBreakpoint: toggleBreakpoint,
                  onFollow: followAddress,
                }}
              />
            ) : (
              <div className="p-3 text-xs text-muted-foreground">
                {paused ? "No disassembly." : "Pause to debug."}
              </div>
            )}
          </TabsContent>

          <TabsContent
            value="registers"
            className="absolute inset-0 overflow-auto p-2"
          >
            <div className="grid grid-cols-2 gap-x-6 gap-y-0.5 font-mono text-xs">
              {registers.map((r) => (
                <div key={r.name} className="flex justify-between">
                  <span className="text-muted-foreground">{r.name}</span>
                  <span>{r.value}</span>
                </div>
              ))}
            </div>
          </TabsContent>

          <TabsContent
            value="stack"
            className="absolute inset-0 overflow-auto font-mono text-xs"
          >
            {frames.map((f, i) => (
              <button
                key={i}
                onClick={() => followAddress(BigInt(f.ip))}
                className="flex w-full justify-between border-b border-border/50 px-3 py-1 text-left hover:bg-accent/50"
              >
                <span>{fmt(BigInt(f.ip))}</span>
                <span className="text-muted-foreground">{f.module || "?"}</span>
              </button>
            ))}
          </TabsContent>

          <TabsContent
            value="breakpoints"
            className="absolute inset-0 overflow-auto font-mono text-xs"
          >
            {breakpoints.length === 0 ? (
              <div className="p-3 text-muted-foreground">No breakpoints.</div>
            ) : (
              breakpoints.map((b) => (
                <div
                  key={b.address}
                  className="flex items-center justify-between border-b border-border/50 px-3 py-1"
                >
                  <button
                    className="hover:underline"
                    onClick={() => followAddress(BigInt(b.address))}
                  >
                    {fmt(BigInt(b.address))}
                  </button>
                  <button
                    className="text-red-400 hover:underline"
                    onClick={async () =>
                      setBreakpoints(
                        await dbg.clearBreakpoint(emulator, BigInt(b.address)),
                      )
                    }
                  >
                    remove
                  </button>
                </div>
              ))
            )}
          </TabsContent>

          <TabsContent
            value="threads"
            className="absolute inset-0 overflow-auto font-mono text-xs"
          >
            {threads.map((t) => (
              <div
                key={t.id}
                className={cn(
                  "flex justify-between border-b border-border/50 px-3 py-1",
                  t.active && "bg-accent",
                )}
              >
                <span>tid {t.id}</span>
                <span className="text-muted-foreground">
                  {fmt(BigInt(t.ip))}
                </span>
              </div>
            ))}
          </TabsContent>

          <TabsContent
            value="modules"
            className="absolute inset-0 overflow-auto font-mono text-xs"
          >
            {modules.map((m) => (
              <button
                key={m.base}
                onClick={() => followAddress(BigInt(m.entry || m.base))}
                className="flex w-full justify-between border-b border-border/50 px-3 py-1 text-left hover:bg-accent/50"
              >
                <span className="truncate">{m.name}</span>
                <span className="text-muted-foreground">
                  {fmt(BigInt(m.base))}
                </span>
              </button>
            ))}
          </TabsContent>

          <TabsContent value="cfg" className="absolute inset-0">
            <CfgView
              emulator={emulator}
              paused={paused}
              entry={selected ?? rip}
              rip={rip}
              generation={generation}
              onSelect={followAddress}
            />
          </TabsContent>

          <TabsContent value="memory" className="absolute inset-0 flex">
            <HexViewer
              key={`${generation}|${tab === "memory"}`}
              emulator={emulator}
              viewBase={rsp !== BigInt(0) ? rsp : rip}
              viewSize={0x1000}
              paused={paused}
              scrollToOffset={0}
            />
          </TabsContent>

          <TabsContent value="script" className="absolute inset-0">
            <ScriptConsole emulator={emulator} />
          </TabsContent>
        </div>
      </Tabs>
    </div>
  );
}
