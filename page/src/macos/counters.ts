// The live-counters vocabulary: every field src/macos-web/main.cpp's "status"/"stream"/"exited"/"stopped"
// events and page/public/macos-emulator-worker.js's own "frame"/"input-dropped" messages carry, plus the
// two counters (input sent, keys with no VK) that only ever exist on the page side because nothing about
// them reaches the guest. This is the standalone page's own 15-row table
// (src/macos-web/app.js:1058-1076), ported field-for-field so page/src/macos/guest-screen.tsx can render
// the same set without a per-counter switch of its own.

export interface StreamCounters {
  drawn: number;
  sent: number;
  dropped: number;
  presents: number;
  threads: number;
  guestWindows: number;
  inputSent: number;
  inputDelivered: number;
  inputDropped: number;
  unmappedKeys: number;
  syscalls: number;
  credit: number | null;
  stopped: string | null;
  idle: boolean;
  attached: boolean | null;
  attachReason: string | null;
}

export function initialCounters(): StreamCounters {
  return {
    drawn: 0,
    sent: 0,
    dropped: 0,
    presents: 0,
    threads: 0,
    guestWindows: 0,
    inputSent: 0,
    inputDelivered: 0,
    inputDropped: 0,
    unmappedKeys: 0,
    syscalls: 0,
    credit: null,
    stopped: null,
    idle: false,
    attached: null,
    attachReason: null,
  };
}

export function recordFrameDrawn(counters: StreamCounters): StreamCounters {
  return { ...counters, drawn: counters.drawn + 1 };
}

export function recordInputSent(counters: StreamCounters): StreamCounters {
  return { ...counters, inputSent: counters.inputSent + 1 };
}

export function recordUnmappedKey(counters: StreamCounters): StreamCounters {
  return { ...counters, unmappedKeys: counters.unmappedKeys + 1 };
}

export function recordInputDropped(
  counters: StreamCounters,
  count: number,
): StreamCounters {
  return { ...counters, inputDropped: Number(count) || 0 };
}

// The "status"/"stream"/"idle"/"exited"/"stopped" events fire every 500ms
// (src/macos-web/main.cpp's STATUS_INTERVAL_MS) or on a state change, and each one only ever touches its
// own slice of StreamCounters -- an unrecognised t leaves counters untouched rather than resetting it.
export function applyStatusEvent(
  counters: StreamCounters,
  event: Record<string, unknown>,
): StreamCounters {
  switch (event.t) {
    case "status":
      return {
        ...counters,
        sent: Number(event.frames) || 0,
        dropped: Number(event.dropped) || 0,
        presents: Number(event.presents) || 0,
        guestWindows: Number(event.windows) || 0,
        threads: Number(event.threads) || 0,
        inputDelivered: Number(event.input) || 0,
        syscalls: Number(event.syscalls) || 0,
        credit:
          event.credit !== undefined ? Number(event.credit) : counters.credit,
      };

    case "stream": {
      const attached = Boolean(Number(event.attached));
      return {
        ...counters,
        attached,
        attachReason: attached ? null : String(event.reason || ""),
      };
    }

    case "idle":
      return { ...counters, idle: true };

    case "exited":
      return { ...counters, stopped: `exited ${Number(event.status) | 0}` };

    case "stopped":
      return { ...counters, stopped: String(event.reason || "stopped") };

    default:
      return counters;
  }
}

// A notice is a one-line banner, not a counter -- most event kinds have nothing to say here.
// `undefined` means leave the current banner alone; `null` clears it.
export function statusEventNotice(
  event: Record<string, unknown>,
): string | null | undefined {
  switch (event.t) {
    case "idle":
      return `${String(event.reason || "idle")}. Click the desktop above.`;

    case "dyld-error":
      return `dyld refused the launch: ${String(event.message || "")}`;

    case "gui": {
      const boundCount = Number(event.bound) || 0;
      if (boundCount === 0) {
        return "window server: 0 routines intercepted -- the guest will reach the real SkyLight, which is not there.";
      }
      return undefined;
    }

    case "fatal":
      return `failed: ${String(event.message || "")}`;

    default:
      return undefined;
  }
}

export interface CounterRow {
  key: string;
  label: string;
  warn: (counters: StreamCounters) => boolean;
  value: (counters: StreamCounters, fps: number) => string | null;
}

// One row per standalone-page table entry (src/macos-web/app.js:1058-1076): the first ten
// (drawn..syscalls) are its unconditional `rows` array and always show, including as a bare "0" --
// zero is information ("nothing was refused") and a hidden row would instead read as "not tracked".
// The last five are its `if (...) rows.push(...)` guards and only show once the run has produced
// something worth reporting.
export const COUNTER_ROWS: CounterRow[] = [
  {
    key: "drawn",
    label: "frames drawn",
    warn: () => false,
    value: (c) => String(c.drawn),
  },
  {
    key: "sent",
    label: "frames sent",
    warn: () => false,
    value: (c) => String(c.sent),
  },
  {
    key: "dropped",
    label: "frames refused",
    warn: (c) => c.dropped > 0,
    value: (c) => String(c.dropped),
  },
  {
    key: "fps",
    label: "frame rate",
    warn: () => false,
    value: (_c, fps) => (fps ? `${fps.toFixed(1)} /s` : "—"),
  },
  {
    key: "presents",
    label: "present count",
    warn: () => false,
    value: (c) => String(c.presents),
  },
  {
    key: "guestWindows",
    label: "guest windows",
    warn: () => false,
    value: (c) => String(c.guestWindows),
  },
  {
    key: "threads",
    label: "guest threads",
    warn: () => false,
    value: (c) => String(c.threads),
  },
  {
    key: "inputSent",
    label: "input sent",
    warn: () => false,
    value: (c) => String(c.inputSent),
  },
  {
    key: "inputDelivered",
    label: "input reached guest side",
    warn: () => false,
    value: (c) => String(c.inputDelivered),
  },
  {
    key: "syscalls",
    label: "guest syscalls",
    warn: () => false,
    value: (c) => String(c.syscalls),
  },
  {
    key: "credit",
    label: "lowest page credit",
    warn: () => false,
    value: (c) => (c.credit !== null ? String(c.credit) : null),
  },
  {
    key: "inputDropped",
    label: "input dropped",
    warn: () => true,
    value: (c) => (c.inputDropped > 0 ? String(c.inputDropped) : null),
  },
  {
    key: "unmappedKeys",
    label: "keys with no VK",
    warn: () => true,
    value: (c) => (c.unmappedKeys > 0 ? String(c.unmappedKeys) : null),
  },
  {
    key: "attached",
    label: "stream",
    warn: () => true,
    value: (c) =>
      c.attached === false
        ? c.attachReason
          ? `not attached (${c.attachReason})`
          : "not attached"
        : null,
  },
  {
    key: "stopped",
    label: "stop reason",
    warn: () => true,
    value: (c) => c.stopped,
  },
];
