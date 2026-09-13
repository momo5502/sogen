// The macOS trace, rendered from the module's own event vocabulary instead of dumped into the terminal
// as NDJSON. src/macos-web/main.cpp emits one JSON object per line and page/public/macos-emulator-worker
// forwards each as a "macos-status" message; this is what turns them back into readable lines.
//
// Filter semantics are src/macos-web/app.js's, so the two surfaces behave the same: a case-insensitive
// substring over the rendered text, a checkbox per kind, "failed only" scoped to syscalls, and detail
// rows that follow the call they belong to. The six syscall categories are that page's categorise() and
// its swatch colours verbatim; the seven beyond them are kinds the standalone page renders as
// unfilterable informational rows and this one can switch off.

import React from "react";
import { List, ListImperativeAPI, type RowComponentProps } from "react-window";
import { ArrowDown } from "react-bootstrap-icons";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";
import { Emulator } from "@/emulator";

export type TraceKind =
  | "file"
  | "memory"
  | "mach"
  | "process"
  | "signal"
  | "other"
  | "log"
  | "output"
  | "module"
  | "runtime"
  | "input"
  | "result"
  | "error";

interface KindDescriptor {
  kind: TraceKind;
  label: string;
  swatch: string;
  title: string;
}

// The first six are src/macos-web/index.html's own checkboxes, in its order and with app.css's colours.
const KINDS: KindDescriptor[] = [
  {
    kind: "file",
    label: "file",
    swatch: "#4ec98a",
    title: "File and descriptor syscalls",
  },
  {
    kind: "memory",
    label: "memory",
    swatch: "#5aa9ff",
    title: "Mapping and protection syscalls",
  },
  {
    kind: "mach",
    label: "mach",
    swatch: "#b98cff",
    title: "Mach traps and kernel RPC",
  },
  {
    kind: "process",
    label: "process",
    swatch: "#e2b341",
    title: "Process, thread and identity syscalls",
  },
  {
    kind: "signal",
    label: "signal",
    swatch: "#f2685f",
    title: "Signal syscalls",
  },
  {
    kind: "other",
    label: "other",
    swatch: "#3a4757",
    title: "Everything else the guest called",
  },
  {
    kind: "log",
    label: "log",
    swatch: "#00adf7",
    title:
      "The emulator's own logger, when a run is started with Verbose logging",
  },
  {
    kind: "output",
    label: "stdout",
    swatch: "#ececec",
    title: "What the guest wrote to stdout and stderr",
  },
  {
    kind: "module",
    label: "modules",
    swatch: "#9750dd",
    title: "Images the guest mapped",
  },
  {
    kind: "runtime",
    label: "runtime",
    swatch: "#86c000",
    title: "Launch, shared cache, window server and frame stream",
  },
  {
    kind: "input",
    label: "input",
    swatch: "#ffb940",
    title: "Input the emulator took and no guest path carried further",
  },
  {
    kind: "result",
    label: "result",
    swatch: "#3a96dd",
    title: "How the run ended, and its memory and instruction counts",
  },
  {
    kind: "error",
    label: "errors",
    swatch: "#ff3131",
    title: "Failures the run reported",
  },
];

const SWATCH = new Map(KINDS.map((entry) => [entry.kind, entry.swatch]));

type Tone = "plain" | "muted" | "info" | "warn" | "bad" | "good";

const TONE_CLASS: Record<Tone, string> = {
  plain: "",
  muted: "terminal-dark-gray",
  info: "terminal-cyan",
  warn: "terminal-yellow",
  bad: "terminal-red",
  good: "terminal-green",
};

interface TraceRow {
  kind: TraceKind;
  tone: Tone;
  text: string;
  needle: string;
  // A syscall header, or a detail/failure line belonging to the header above it. Both carry their call's
  // category rather than one of their own, which is what makes a filtered group disappear whole.
  call: boolean;
  detail: boolean;
  failed: boolean;
}

// src/macos-web/app.js's categorise(), unchanged. Prefix matching rather than a table of every name: the
// naming is consistent enough that a new syscall lands in the right group without anyone remembering to
// add it, and the cost of a wrong guess is a colour.
function categorise(name: string): TraceKind {
  if (/^(_kernelrpc|mach_|task_|thread_|host_|semaphore|bootstrap)/.test(name))
    return "mach";
  if (/(mmap|munmap|mprotect|madvise|shared_region|vm_|brk|mincore)/.test(name))
    return "memory";
  if (
    /(open|close|read|write|stat|lseek|access|fcntl|dup|getdirent|unlink|rename|mkdir|rmdir|link|attrlist|fsgetpath|pread|pwrite|ioctl)/.test(
      name,
    )
  )
    return "file";
  if (/(sig|kill|abort)/.test(name)) return "signal";
  if (
    /(pid|uid|gid|proc|exec|exit|fork|thread|audit|rlimit|sysctl|csrctl|mac_syscall)/.test(
      name,
    )
  )
    return "process";
  return "other";
}

function bytes(n: number): string {
  if (!Number.isFinite(n)) return "?";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1048576).toFixed(1)} MiB`;
  return `${(n / 1073741824).toFixed(2)} GiB`;
}

function text(value: unknown): string {
  return value === undefined || value === null ? "" : String(value);
}

function number(value: unknown): number {
  return Number(value) || 0;
}

function trimEol(value: string): string {
  return value.replace(/[\r\n]+$/, "");
}

interface Emitted {
  kind: TraceKind;
  tone: Tone;
  text: string;
  call?: boolean;
  detail?: boolean;
}

// The one place that knows the module's vocabulary. Everything it returns becomes exactly one row;
// returning null is how a kind that belongs somewhere else on the page stays out of the trace.
function describe(
  event: Record<string, unknown>,
  state: TraceState,
): Emitted | null {
  switch (event.t) {
    case "started":
      return {
        kind: "runtime",
        tone: "info",
        text: `sogen ${text(event.backend)} — launching ${text(event.exe)}`,
      };

    case "image": {
      if (!number(event.readable))
        return {
          kind: "runtime",
          tone: "warn",
          text: "the image could not be read",
        };
      const dylinker = text(event.dylinker);
      const parts = [
        `image: ${bytes(number(event.bytes))}`,
        number(event.arm64e) ? "arm64e" : "arm64",
        `entry ${text(event.entry)}`,
        dylinker ? `dylinker ${dylinker}` : "static",
      ];
      if (event.error) parts.push(`error: ${text(event.error)}`);
      return { kind: "runtime", tone: "info", text: parts.join(", ") };
    }

    case "module":
      return {
        kind: "module",
        tone: "muted",
        text: `Mapped ${text(event.name)} at ${text(event.base)} (${bytes(number(event.size))})`,
      };

    case "cache":
      return {
        kind: "runtime",
        tone: "info",
        text: `Shared cache mapped: ${number(event.mappings)} mappings, ${number(event.rebased)} pointers rebased.`,
      };

    case "gui": {
      const bound = number(event.bound);
      const unbound = number(event.unbound);
      return {
        kind: bound > 0 ? "runtime" : "error",
        tone: bound > 0 ? "info" : "bad",
        text:
          `Window server: ${bound} of ${number(event.registered)} routines intercepted` +
          (unbound ? `, ${unbound} not exported on this system` : "") +
          (bound
            ? "."
            : " — the guest will reach the real SkyLight, which is not there."),
      };
    }

    case "stream":
      return number(event.attached)
        ? {
            kind: "runtime",
            tone: "info",
            text: `Streaming the composed desktop every ${number(event.interval)} ms while the guest runs.`,
          }
        : {
            kind: "error",
            tone: "bad",
            text: `Frames are not being streamed: ${text(event.reason)}`,
          };

    case "idle":
      return {
        kind: "runtime",
        tone: "info",
        text: `${text(event.reason)}. Click the desktop.`,
      };

    case "range":
      return {
        kind: "runtime",
        tone: "muted",
        text:
          `range probe ${text(event.path)} +${number(event.offset)}: ` +
          `${number(event.read)} of ${number(event.requested)} bytes read from ${bytes(number(event.size))}, head ${text(event.head)}`,
      };

    // Only the first event of each message is listed, and only when nothing carried it further: a few
    // seconds of mouse movement is thousands of identical rows, and the counts beside the desktop are the
    // part that keeps changing. src/macos-web/app.js does the same.
    case "input": {
      if (number(event.delivered)) return null;
      const message = text(event.message);
      if (state.reportedInput.has(message)) return null;
      state.reportedInput.add(message);

      const code = number(event.code);
      const lparam = number(event.lparam);
      const where =
        code >= 0x0200
          ? `at ${lparam & 0xffff},${(lparam >>> 16) & 0xffff}`
          : `vk ${number(event.wparam)}`;
      return {
        kind: "input",
        tone: "warn",
        text:
          `${message} ${where} reached the emulator and no guest-side path carried it further. ` +
          `Further undelivered ${message} events are counted beside the desktop rather than listed.`,
      };
    }

    case "syscall": {
      ++state.syscallIndex;
      state.callKind = categorise(text(event.name));
      return {
        kind: state.callKind,
        tone: "plain",
        call: true,
        text: `#${state.syscallIndex} ${text(event.name)}  0x${number(event.id).toString(16)}  ${text(event.pc)}`,
      };
    }

    case "detail":
      return {
        kind: state.callKind,
        tone: "muted",
        detail: true,
        text: event.label
          ? `    ${text(event.label)}: ${text(event.value)}`
          : `    ${text(event.value)}`,
      };

    case "failed":
      return {
        kind: state.callKind,
        tone: "bad",
        detail: true,
        text: event.name
          ? `    Failed: ${text(event.name)} (${number(event.errno)})`
          : `    Failed: errno ${number(event.errno)}`,
      };

    // Trailing newline stripped, unlike src/macos-web/app.js: every printf the guest makes carries one,
    // and here a row is a fixed-height line -- keeping it would leave a blank line per write in the
    // exported log and trailing whitespace in every rendered row.
    case "stdout":
      return { kind: "output", tone: "plain", text: trimEol(text(event.text)) };

    case "stderr":
      return { kind: "output", tone: "warn", text: trimEol(text(event.text)) };

    // The emulator's own logger, routed here by --log rather than to a terminal it has no colours for.
    // The level is the colour it was printed in; only red and yellow read as something gone wrong.
    case "log": {
      const level = text(event.level);
      const tone: Tone =
        level === "error"
          ? "bad"
          : level === "warn"
            ? "warn"
            : level === "success"
              ? "good"
              : "info";
      return { kind: "log", tone, text: text(event.text) };
    }

    // Not the module's: the worker's own attach diagnostics, plus anything emscripten printed that was
    // not a JSON line. A failing one is the range bridge reporting a read it could not answer.
    case "line":
      return number(event.error)
        ? { kind: "error", tone: "bad", text: text(event.text) }
        : { kind: "runtime", tone: "muted", text: text(event.text) };

    case "memory":
      return {
        kind: "result",
        tone: "muted",
        text:
          `memory: ${bytes(number(event.committed))} committed across ${number(event.regions)} regions, ` +
          `${bytes(number(event.reserved))} reserved, ${bytes(number(event.host))} of host heap`,
      };

    case "counters":
      return number(event.counted)
        ? {
            kind: "result",
            tone: "muted",
            text: `${number(event.instructions)} instructions in ${number(event.blocks)} basic blocks`,
          }
        : {
            kind: "result",
            tone: "muted",
            text: "this build does not count executed instructions",
          };

    case "exited":
      return {
        kind: "result",
        tone: number(event.status) ? "warn" : "good",
        text: `exited with status ${number(event.status)}`,
      };

    case "stopped": {
      const detail = event.detail ? ` — ${text(event.detail)}` : "";
      return {
        kind: "result",
        tone: "bad",
        text: `Stopped: ${text(event.reason) || "unknown"}${detail} (pc ${text(event.pc) || "?"})`,
      };
    }

    case "dyld-error":
      return {
        kind: "error",
        tone: "bad",
        text: `dyld refused the launch: ${text(event.message)}`,
      };

    case "frame":
      return { kind: "error", tone: "bad", text: `    ${text(event.text)}` };

    case "fatal":
      return {
        kind: "error",
        tone: "bad",
        text: `failed: ${text(event.message)}`,
      };

    // Twice a second for the length of a run, and page/src/macos/guest-screen.tsx already renders every
    // field of it beside the desktop, live. A row per tick would be 2,400 of them in a twenty-minute run
    // saying what the panel next to them already says. The only kind that is deliberately not a row.
    case "status":
      return null;

    // Shown raw rather than dropped. A kind this page has never heard of is one src/macos-web/main.cpp
    // grew after it, and silently swallowing it is the same failure this view exists to end: a diagnostic
    // that is emitted and invisible. Unstyled and obviously new beats absent.
    default: {
      const json = JSON.stringify(event);
      return {
        kind: "runtime",
        tone: "warn",
        text: `unhandled event ${text(event.t)}: ${json.length > 400 ? `${json.slice(0, 400)}…` : json}`,
      };
    }
  }
}

interface TraceState {
  syscallIndex: number;
  callKind: TraceKind;
  reportedInput: Set<string>;
}

function freshState(): TraceState {
  return { syscallIndex: 0, callKind: "other", reportedInput: new Set() };
}

// A twenty-minute run against a real .app produces on the order of a million events. Past this the
// oldest go, because a trace that costs the tab its memory is a trace nobody gets to read.
const ROW_LIMIT = 200000;
const ROW_DROP = 40000;

// Two views of the same rows, both maintained as they arrive. `calls` is the trace without its decoded
// arguments, which is what the "arguments" switch shows when it is off -- and it is off by default, so
// deriving it by filtering would put an O(rows) scan on the default path of every 150 ms flush.
// `total` counts rows ever appended, including the ones the cap dropped.
interface TraceModel {
  rows: TraceRow[];
  calls: TraceRow[];
  total: number;
}

function emptyModel(): TraceModel {
  return { rows: [], calls: [], total: 0 };
}

// The trace arrives in bursts and React cannot usefully paint faster than this. Every appended row is
// held until the next tick, which is also how often the visible set is recomputed.
const FLUSH_INTERVAL_MS = 150;

const ROW_HEIGHT = 20;

function TraceRowView({
  ariaAttributes,
  rows,
  index,
  style,
}: RowComponentProps<{ rows: TraceRow[] }>) {
  const row = rows[index];
  return (
    <span
      className={cn(
        "flex items-center gap-1.5 whitespace-nowrap",
        TONE_CLASS[row.tone],
      )}
      style={style}
      {...ariaAttributes}
    >
      <span
        className="inline-block h-3 w-[3px] shrink-0 rounded-sm"
        style={{ background: row.call ? SWATCH.get(row.kind) : "transparent" }}
      />
      {/* Ellipsised rather than clipped, with the whole line on hover: a decoded buffer preview can be
          longer than any pane, and react-window gives every row the list's own width. */}
      <span
        className={cn(
          "truncate",
          row.call && "font-semibold",
          row.call && row.failed && "terminal-red",
        )}
        title={row.text}
      >
        {row.text}
      </span>
    </span>
  );
}

export interface MacosTraceViewHandle {
  getLines(): string[];
}

export interface MacosTraceViewProps {
  emulator?: Emulator;
}

export const MacosTraceView = React.forwardRef<
  MacosTraceViewHandle,
  MacosTraceViewProps
>(function MacosTraceView({ emulator }, handle) {
  const stateRef = React.useRef<TraceState>(freshState());
  const pendingRef = React.useRef<TraceRow[]>([]);
  const currentCallRef = React.useRef<TraceRow | null>(null);
  const flushTimerRef = React.useRef<number | null>(null);

  const [model, setModel] = React.useState<TraceModel>(emptyModel);
  const [needle, setNeedle] = React.useState("");
  const [enabled, setEnabled] = React.useState<Set<TraceKind>>(
    () => new Set(KINDS.map((entry) => entry.kind)),
  );
  const [failedOnly, setFailedOnly] = React.useState(false);
  const [showArguments, setShowArguments] = React.useState(false);
  const [autoScroll, setAutoScroll] = React.useState(true);

  const containerRef = React.useRef<HTMLDivElement>(null);
  const listRef = React.useRef<ListImperativeAPI>(null);
  const [size, setSize] = React.useState({ width: 0, height: 0 });

  // The header says how many rows the cap dropped; an exported file that did not would look complete
  // while being a tail, which is worse than a short log.
  React.useImperativeHandle(
    handle,
    () => ({
      getLines: () => {
        const lines = model.rows.map((row) => row.text);
        const dropped = model.total - model.rows.length;
        return dropped > 0
          ? [
              `# ${dropped} earlier line(s) dropped: the trace is capped at ${ROW_LIMIT} rows and this is the tail.`,
              ...lines,
            ]
          : lines;
      },
    }),
    [model],
  );

  React.useEffect(() => {
    const element = containerRef.current;
    if (!element) return;

    const observer = new ResizeObserver(() => {
      setSize({ width: element.clientWidth, height: element.clientHeight });
    });
    observer.observe(element);
    setSize({ width: element.clientWidth, height: element.clientHeight });
    return () => observer.disconnect();
  }, []);

  const flush = React.useCallback(() => {
    flushTimerRef.current = null;

    const pending = pendingRef.current;
    if (!pending.length) return;
    pendingRef.current = [];

    // Both arrays are advanced together, in one updater, so the cap can drop from each consistently:
    // trimming them separately would need a second pass to work out which of the dropped rows were
    // details.
    setModel((current) => {
      let rows = current.rows.concat(pending);
      let calls = current.calls.concat(pending.filter((row) => !row.detail));

      if (rows.length > ROW_LIMIT) {
        const drop = rows.length - ROW_LIMIT + ROW_DROP;
        let droppedCalls = 0;
        for (let i = 0; i < drop; ++i) {
          if (!rows[i].detail) ++droppedCalls;
        }
        rows = rows.slice(drop);
        calls = calls.slice(droppedCalls);
      }

      return { rows, calls, total: current.total + pending.length };
    });
  }, []);

  const append = React.useCallback(
    (event: Record<string, unknown>) => {
      const emitted = describe(event, stateRef.current);
      if (!emitted) return;

      const row: TraceRow = {
        kind: emitted.kind,
        tone: emitted.tone,
        text: emitted.text,
        needle: emitted.text.toLowerCase(),
        call: Boolean(emitted.call),
        detail: Boolean(emitted.detail),
        failed: false,
      };

      // The failure marks the call itself, not just the line under it: a reader scanning a filtered trace
      // has to be able to see which calls did not work, and "failed only" has nothing else to select on.
      // The call row is mutated in place whether it has been flushed yet or not -- flushing concatenates
      // the same objects rather than copying them, so both arrays hold this one.
      if (row.call) currentCallRef.current = row;
      else if (event.t === "failed" && currentCallRef.current)
        currentCallRef.current.failed = true;

      pendingRef.current.push(row);

      if (flushTimerRef.current === null) {
        flushTimerRef.current = window.setTimeout(flush, FLUSH_INTERVAL_MS);
      }
    },
    [flush],
  );

  // Subscribed with addEventListener rather than replacing worker.onmessage: page/src/emulator.ts already
  // owns that property for its own {message, data} envelope, and page/src/macos/guest-screen.tsx listens
  // to the same worker for the frame and counter shapes.
  React.useEffect(() => {
    pendingRef.current = [];
    stateRef.current = freshState();
    currentCallRef.current = null;

    // Resets rather than derives: a new emulator is a new run of a new worker, and the previous run's
    // trace belongs to a session that no longer exists.
    /* eslint-disable react-hooks/set-state-in-effect */
    setModel(emptyModel());
    setAutoScroll(true);
    /* eslint-enable react-hooks/set-state-in-effect */

    const worker = emulator?.worker;
    if (!worker) return;

    const onMessage = (event: MessageEvent) => {
      const data = event.data;
      if (data && data.message === "macos-status")
        append(data.event as Record<string, unknown>);
    };

    worker.addEventListener("message", onMessage);
    return () => {
      worker.removeEventListener("message", onMessage);
      if (flushTimerRef.current !== null)
        window.clearTimeout(flushTimerRef.current);
      flushTimerRef.current = null;
    };
  }, [emulator, append]);

  const allKinds = enabled.size === KINDS.length;

  const visible = React.useMemo(() => {
    // Whichever array the "arguments" switch selects is already the answer when nothing else is filtered,
    // and it is handed back by identity: a run that nobody has filtered yet -- which is every run for its
    // first minutes, and the configuration this view defaults to -- must not rescan a million rows six
    // times a second to learn that they are all still visible.
    if (allKinds && !needle && !failedOnly) {
      return showArguments ? model.rows : model.calls;
    }

    const lowered = needle.toLowerCase();
    const out: TraceRow[] = [];

    // A row is hidden if its text misses the needle, its kind is switched off, or "failed only" is on and
    // it belongs to a call that worked. Detail rows follow their own call, so a group never half-appears.
    // Scanned whole rather than extended per batch because "failed only" depends on a flag a later event
    // sets on an earlier row: a call already appended becomes visible the moment its failure arrives.
    let groupVisible = true;
    for (const row of model.rows) {
      if (row.detail) {
        if (groupVisible && showArguments) out.push(row);
        continue;
      }

      const shown =
        enabled.has(row.kind) &&
        (!lowered || row.needle.includes(lowered)) &&
        (!failedOnly || (row.call && row.failed));

      if (row.call) groupVisible = shown;
      if (shown) out.push(row);
    }

    return out;
  }, [model, enabled, needle, failedOnly, showArguments, allKinds]);

  const scrollToEnd = React.useCallback(() => {
    if (listRef.current && visible.length > 0) {
      listRef.current.scrollToRow({
        index: visible.length - 1,
        behavior: "instant",
      });
    }
    setAutoScroll(true);
  }, [visible.length]);

  React.useEffect(() => {
    if (autoScroll && listRef.current && visible.length > 0) {
      listRef.current.scrollToRow({
        index: visible.length - 1,
        behavior: "instant",
      });
    }
  }, [autoScroll, visible.length]);

  const onScroll = React.useCallback((event: React.UIEvent<HTMLDivElement>) => {
    const element = event.currentTarget;
    setAutoScroll(
      element.scrollTop + element.clientHeight >= element.scrollHeight - 40,
    );
  }, []);

  const toggle = React.useCallback((kind: TraceKind) => {
    setEnabled((current) => {
      const next = new Set(current);
      if (next.has(kind)) next.delete(kind);
      else next.add(kind);
      return next;
    });
  }, []);

  const total = model.total;
  const dropped = total - model.rows.length;

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      {/* One non-wrapping row, above page/src/components/emulation-summary.tsx rather than under it: that
          panel floats over the top-right of this pane at z-49 and would otherwise swallow the clicks on
          whichever filters wrapped beneath it. */}
      <div className="relative z-50 flex flex-nowrap items-center gap-x-3 overflow-x-auto border-b bg-background px-2 py-1 text-xs">
        <span
          className="shrink-0 text-muted-foreground"
          data-testid="macos-trace-count"
        >
          {visible.length === total
            ? `${total} lines`
            : `${visible.length} of ${total} lines`}
          {dropped > 0 ? ` (${dropped} older dropped)` : ""}
        </span>

        <Input
          className="h-7 min-w-48 flex-1 text-xs"
          value={needle}
          onChange={(e) => setNeedle(e.target.value)}
          placeholder="Filter lines (substring, case-insensitive)"
          aria-label="Filter trace lines"
        />

        {KINDS.map((entry) => (
          <label
            key={entry.kind}
            className="flex shrink-0 cursor-pointer items-center gap-1"
            title={entry.title}
          >
            <input
              type="checkbox"
              checked={enabled.has(entry.kind)}
              onChange={() => toggle(entry.kind)}
              data-testid={`macos-trace-kind-${entry.kind}`}
            />
            <span
              className="inline-block h-2 w-2 rounded-sm"
              style={{ background: entry.swatch }}
            />
            {entry.label}
          </label>
        ))}

        <label
          className="flex shrink-0 cursor-pointer items-center gap-1"
          title="Only calls the emulator answered with an error"
        >
          <input
            type="checkbox"
            checked={failedOnly}
            onChange={(e) => setFailedOnly(e.target.checked)}
            data-testid="macos-trace-failed-only"
          />
          failed only
        </label>

        <label
          className="flex shrink-0 cursor-pointer items-center gap-1"
          title="Show the decoded arguments under every call"
        >
          <input
            type="checkbox"
            checked={showArguments}
            onChange={(e) => setShowArguments(e.target.checked)}
            data-testid="macos-trace-arguments"
          />
          arguments
        </label>
      </div>

      <div className="relative min-h-0 flex-1">
        <div
          className="terminal-output absolute inset-0 pl-1"
          ref={containerRef}
        >
          {model.rows.length === 0 ? (
            <div className="p-2 text-xs text-muted-foreground">
              Nothing yet. Start a run — every syscall, module and window-server
              event the emulator reports lands here.
            </div>
          ) : (
            <List
              listRef={listRef}
              overscanCount={30}
              rowComponent={TraceRowView}
              rowCount={visible.length}
              rowProps={{ rows: visible }}
              rowHeight={ROW_HEIGHT}
              onScroll={onScroll}
              style={{ height: size.height, width: size.width }}
            />
          )}
        </div>
        <Button
          title="Scroll to end"
          className={cn(
            "terminal-glass absolute bottom-6 right-6 z-50 transition-opacity duration-50 ease-linear",
            autoScroll && "opacity-0",
          )}
          variant="secondary"
          onClick={scrollToEnd}
        >
          <ArrowDown />
        </Button>
      </div>
    </div>
  );
});
