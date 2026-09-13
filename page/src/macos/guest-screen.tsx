import React from "react";

import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";
import { Emulator } from "@/emulator";
import { Settings } from "@/settings";
import {
  registerRootCache,
  rootCacheState,
  prepareRoot,
  cancelPrepareRoot,
  clearRootCache,
  setRootCacheBypass,
  RootCacheState,
} from "./root-cache";
import {
  StreamCounters,
  initialCounters,
  recordFrameDrawn,
  recordInputSent,
  recordUnmappedKey,
  recordInputDropped,
  applyStatusEvent,
  statusEventNotice,
  COUNTER_ROWS,
} from "./counters";

// Win32 message numbers, because that is what a ui_event carries end to end: the emulator's UI layer is
// shared with the Windows guest path (page/src/web-ui-host.ts speaks the same vocabulary) and the macOS
// side translates out of it, not into it (src/macos-web/app.js:1048).
const WM_MOUSEMOVE = 0x0200;
const WM_LBUTTONDOWN = 0x0201;
const WM_LBUTTONUP = 0x0202;
const WM_RBUTTONDOWN = 0x0204;
const WM_RBUTTONUP = 0x0205;
const WM_MBUTTONDOWN = 0x0207;
const WM_MBUTTONUP = 0x0208;
const WM_MOUSEWHEEL = 0x020a;
const WM_MOUSEHWHEEL = 0x020e;
const WM_KEYDOWN = 0x0100;
const WM_KEYUP = 0x0101;
const WM_CHAR = 0x0102;
const WM_SYSKEYDOWN = 0x0104;
const WM_SYSKEYUP = 0x0105;

const MK_LBUTTON = 0x0001;
const MK_RBUTTON = 0x0002;
const MK_SHIFT = 0x0004;
const MK_CONTROL = 0x0008;
const MK_MBUTTON = 0x0010;

const WHEEL_DELTA = 120;

const VK_BY_CODE: Record<string, number> = {
  Escape: 0x1b,
  Enter: 0x0d,
  NumpadEnter: 0x0d,
  Backspace: 0x08,
  Tab: 0x09,
  Space: 0x20,
  ArrowLeft: 0x25,
  ArrowUp: 0x26,
  ArrowRight: 0x27,
  ArrowDown: 0x28,
  Delete: 0x2e,
  Insert: 0x2d,
  Home: 0x24,
  End: 0x23,
  PageUp: 0x21,
  PageDown: 0x22,
  ShiftLeft: 0x10,
  ShiftRight: 0x10,
  ControlLeft: 0x11,
  ControlRight: 0x11,
  AltLeft: 0x12,
  AltRight: 0x12,
  MetaLeft: 0x5b,
  MetaRight: 0x5c,
  CapsLock: 0x14,
  Semicolon: 0xba,
  Equal: 0xbb,
  Comma: 0xbc,
  Minus: 0xbd,
  Period: 0xbe,
  Slash: 0xbf,
  Backquote: 0xc0,
  BracketLeft: 0xdb,
  Backslash: 0xdc,
  BracketRight: 0xdd,
  Quote: 0xde,
  NumpadMultiply: 0x6a,
  NumpadAdd: 0x6b,
  NumpadSubtract: 0x6d,
  NumpadDecimal: 0x6e,
  NumpadDivide: 0x6f,
};

function virtualKey(code: string): number {
  if (VK_BY_CODE[code] !== undefined) return VK_BY_CODE[code];
  if (/^Key[A-Z]$/.test(code)) return code.charCodeAt(3);
  if (/^Digit[0-9]$/.test(code)) return code.charCodeAt(5);
  if (/^Numpad[0-9]$/.test(code)) return 0x60 + (code.charCodeAt(6) - 0x30);
  if (/^F([1-9]|1[0-2])$/.test(code)) return 0x6f + Number(code.slice(1));
  return 0;
}

function packPoint(x: number, y: number) {
  return (((y & 0xffff) << 16) | (x & 0xffff)) >>> 0;
}

interface DesktopWindow {
  id: number;
  x: number;
  y: number;
  w: number;
  h: number;
  level: number;
  visible: boolean;
}

interface DesktopFrame {
  width: number;
  height: number;
  windows: DesktopWindow[];
}

// The topmost visible window covering the point, so a guest that reads the window field gets the one it
// would have hit. Zero means the desktop itself; null means "no point to hit-test", used for keyboard
// focus.
function windowAt(
  point: { x: number; y: number } | null,
  windows: DesktopWindow[],
): number {
  let best = 0;
  let bestLevel = -Infinity;

  for (const w of windows) {
    if (!w.visible) continue;
    if (
      point &&
      (point.x < w.x ||
        point.y < w.y ||
        point.x >= w.x + w.w ||
        point.y >= w.y + w.h)
    )
      continue;
    if (w.level >= bestLevel) {
      bestLevel = w.level;
      best = w.id;
    }
  }

  return best;
}

function bytes(n: number): string {
  if (!Number.isFinite(n)) return "?";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1048576).toFixed(1)} MiB`;
  return `${(n / 1073741824).toFixed(2)} GiB`;
}

interface Demo {
  path: string;
  label: string;
  needsRoot: boolean;
}

const DEMOS: Demo[] = [
  { path: "/demo/macho_trace_arm64", label: "Trace demo", needsRoot: false },
  { path: "/demo/macho_calcdemo_arm64", label: "GUI demo", needsRoot: true },
  {
    path: "/demo/macho_paintprobe_arm64",
    label: "Paint demo",
    needsRoot: true,
  },
];

export interface MacosGuestScreenProps {
  emulator?: Emulator;
  settings: Settings;
  onRunDemo: (guestPath: string, needsRoot: boolean) => void;
}

export function MacosGuestScreen({
  emulator,
  settings,
  onRunDemo,
}: MacosGuestScreenProps) {
  void settings;

  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const stageRef = React.useRef<HTMLDivElement>(null);
  const desktopRef = React.useRef<DesktopFrame>({
    width: 0,
    height: 0,
    windows: [],
  });
  const reportedInputRef = React.useRef<Set<string>>(new Set());
  const lastFrameAtRef = React.useRef(0);

  const [inputEnabled, setInputEnabled] = React.useState(true);
  const [desktopSize, setDesktopSize] = React.useState({ width: 0, height: 0 });
  const [windows, setWindows] = React.useState<DesktopWindow[]>([]);
  const [fps, setFps] = React.useState(0);
  const [counters, setCounters] =
    React.useState<StreamCounters>(initialCounters);
  const [notice, setNotice] = React.useState<string | null>(null);

  const running = !!emulator;

  const [cache, setCache] = React.useState<RootCacheState | null>(null);
  const [cacheError, setCacheError] = React.useState<string | null>(null);
  const [preparing, setPreparing] = React.useState(false);
  const [prepareProgress, setPrepareProgress] = React.useState<{
    done: number;
    total: number;
  } | null>(null);

  const refreshCache = React.useCallback(() => {
    rootCacheState()
      .then(setCache)
      .catch((error) => setCacheError(String(error)));
  }, []);

  React.useEffect(() => {
    registerRootCache()
      .then((state) => {
        setCache(state);
        setCacheError(null);
      })
      .catch((error) => setCacheError(String(error)));
  }, []);

  const [startingDemo, setStartingDemo] = React.useState<string | null>(null);

  // registerRootCache() is memoized, so this awaits the *same* controllerchange wait the mount effect
  // above kicked off rather than a second one -- near-instant once that has resolved, which on a repeat
  // visit it already has. Without this, a fast click on a genuine first visit could create the emulator
  // Worker before the page is controlled, and that worker would bypass the cache for its whole run.
  // A failed registration is not a reason to refuse the run -- the guest still reads the root fine over
  // plain network Range requests, just without the local cache -- but it must not be silent: a click that
  // visibly does nothing, or one that quietly starts an uncached multi-gigabyte run with no explanation
  // for why it is slow, are both worse than a warning banner the user can actually act on.
  const runDemo = React.useCallback(
    async (guestPath: string, needsRoot: boolean) => {
      setStartingDemo(guestPath);
      try {
        await registerRootCache();
      } catch (error) {
        setNotice(
          `local root cache unavailable (${error instanceof Error ? error.message : String(error)}) -- running uncached.`,
        );
      } finally {
        setStartingDemo(null);
      }
      onRunDemo(guestPath, needsRoot);
    },
    [onRunDemo],
  );

  const handlePrepare = React.useCallback(async () => {
    setPreparing(true);
    setPrepareProgress({ done: 0, total: 0 });
    try {
      await prepareRoot((done, total) => setPrepareProgress({ done, total }));
    } catch (error) {
      setCacheError(String(error));
    } finally {
      setPreparing(false);
      setPrepareProgress(null);
      refreshCache();
    }
  }, [refreshCache]);

  const handleClear = React.useCallback(async () => {
    try {
      await clearRootCache();
    } catch (error) {
      setCacheError(String(error));
    }
    refreshCache();
  }, [refreshCache]);

  const handleToggleCache = React.useCallback(
    async (checked: boolean) => {
      try {
        await setRootCacheBypass(!checked);
      } catch (error) {
        setCacheError(String(error));
      }
      refreshCache();
    },
    [refreshCache],
  );

  // The composed desktop, with the window rectangles the guest asked for drawn over it. Pixels arrive as
  // a transferred raw RGBA buffer rather than an encoded image: at a screenshot's size a PNG is already
  // the size of its pixels once the encoder emits stored deflate blocks, and a decode per frame buys
  // nothing a putImageData does not already do.
  const drawFrame = React.useCallback(
    (data: {
      width: number;
      height: number;
      windows: DesktopWindow[];
      pixels: Uint8Array;
    }) => {
      const width = Number(data.width) || 0;
      const height = Number(data.height) || 0;
      const expected = width * height * 4;
      if (
        !width ||
        !height ||
        !data.pixels ||
        data.pixels.byteLength !== expected
      )
        return;

      const canvas = canvasRef.current;
      if (!canvas) return;

      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
      }

      // alpha:false so a translucent window shows what the compositor put behind it rather than the page.
      const context = canvas.getContext("2d", { alpha: false });
      if (context) {
        const pixels = new Uint8ClampedArray(
          data.pixels.buffer as ArrayBuffer,
          data.pixels.byteOffset,
          expected,
        );
        context.putImageData(new ImageData(pixels, width, height), 0, 0);
      }

      desktopRef.current = { width, height, windows: data.windows };
      setDesktopSize({ width, height });
      setWindows(data.windows);

      const now = performance.now();
      if (lastFrameAtRef.current) {
        setFps(1000 / Math.max(1, now - lastFrameAtRef.current));
      }
      lastFrameAtRef.current = now;

      setCounters(recordFrameDrawn);
    },
    [],
  );

  const handleStatusEvent = React.useCallback(
    (event: Record<string, unknown>) => {
      setCounters((c) => applyStatusEvent(c, event));

      const notice = statusEventNotice(event);
      if (notice !== undefined) setNotice(notice);
    },
    [],
  );

  // Subscribed with addEventListener rather than replacing worker.onmessage: page/src/emulator.ts already
  // owns that property for its own {message, data} envelope, and this listens for the frame/status shapes
  // the worker forwards alongside it rather than instead of it.
  React.useEffect(() => {
    const worker = emulator?.worker;
    desktopRef.current = { width: 0, height: 0, windows: [] };
    reportedInputRef.current = new Set();
    lastFrameAtRef.current = 0;

    // Resets rather than derives: a new emulator is a new run of a new worker, and the previous run's
    // frame count, windows and notice belong to a session that no longer exists.
    /* eslint-disable react-hooks/set-state-in-effect */
    setDesktopSize({ width: 0, height: 0 });
    setWindows([]);
    setCounters(initialCounters());
    setNotice(null);
    /* eslint-enable react-hooks/set-state-in-effect */

    if (!worker) {
      return;
    }

    const onMessage = (event: MessageEvent) => {
      const data = event.data;
      if (!data) return;

      if (data.t === "frame") {
        drawFrame(data);
        worker.postMessage({ t: "frame-ack" });
        return;
      }

      if (data.t === "input-dropped") {
        setCounters((c) => recordInputDropped(c, Number(data.count) || 0));
        return;
      }

      if (data.message === "macos-status") {
        handleStatusEvent(data.event as Record<string, unknown>);
      }
    };

    worker.addEventListener("message", onMessage);
    return () => worker.removeEventListener("message", onMessage);
  }, [emulator, drawFrame, handleStatusEvent]);

  const sendInput = React.useCallback(
    (target: number, message: number, wParam: number, lParam: number) => {
      const worker = emulator?.worker;
      if (!worker || !inputEnabled) return;
      worker.postMessage({
        t: "input",
        event: { window: target, message, wParam, lParam },
      });
      setCounters(recordInputSent);
    },
    [emulator, inputEnabled],
  );

  // Desktop coordinates, not CSS ones: the canvas is scaled to fit the panel and the guest only ever knew
  // about the desktop it was given.
  const desktopPoint = React.useCallback((clientX: number, clientY: number) => {
    const canvas = canvasRef.current;
    const desktop = desktopRef.current;
    if (!canvas || !desktop.width || !desktop.height) return null;

    const box = canvas.getBoundingClientRect();
    if (!box.width || !box.height) return null;

    const x = Math.round(((clientX - box.left) / box.width) * desktop.width);
    const y = Math.round(((clientY - box.top) / box.height) * desktop.height);

    return {
      x: Math.max(0, Math.min(desktop.width - 1, x)),
      y: Math.max(0, Math.min(desktop.height - 1, y)),
    };
  }, []);

  function modifierKeys(event: React.MouseEvent | React.WheelEvent): number {
    let keys = 0;
    if (event.shiftKey) keys |= MK_SHIFT;
    if (event.ctrlKey) keys |= MK_CONTROL;
    if (typeof event.buttons === "number") {
      if (event.buttons & 1) keys |= MK_LBUTTON;
      if (event.buttons & 2) keys |= MK_RBUTTON;
      if (event.buttons & 4) keys |= MK_MBUTTON;
    }
    return keys;
  }

  // lParam is the point in the TARGET WINDOW's client space, not the desktop's: the guest side adds the
  // window origin back, the same way sdl_ui_backend::map_window_point does. Sending the desktop point
  // here lands every click at twice its offset from the window.
  const sendMouse = React.useCallback(
    (clientX: number, clientY: number, message: number, wParam: number) => {
      const point = desktopPoint(clientX, clientY);
      if (!point) return;

      const desktop = desktopRef.current;
      const target = windowAt(point, desktop.windows);
      const window = target
        ? desktop.windows.find((w) => w.id === target)
        : null;
      const x = window ? point.x - window.x : point.x;
      const y = window ? point.y - window.y : point.y;

      sendInput(target, message, wParam, packPoint(x, y));
    },
    [desktopPoint, sendInput],
  );

  const onPointerDown = React.useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      stageRef.current?.focus();
      if (!running || !inputEnabled) return;
      event.preventDefault();
      event.currentTarget.setPointerCapture(event.pointerId);

      const down = [WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN][
        event.button
      ];
      if (down === undefined) return;
      sendMouse(event.clientX, event.clientY, down, modifierKeys(event));
    },
    [running, inputEnabled, sendMouse],
  );

  const onPointerUp = React.useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      if (event.currentTarget.hasPointerCapture(event.pointerId)) {
        event.currentTarget.releasePointerCapture(event.pointerId);
      }
      const up = [WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP][event.button];
      if (up === undefined) return;
      sendMouse(event.clientX, event.clientY, up, modifierKeys(event));
    },
    [sendMouse],
  );

  const onPointerMove = React.useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      sendMouse(
        event.clientX,
        event.clientY,
        WM_MOUSEMOVE,
        modifierKeys(event),
      );
    },
    [sendMouse],
  );

  const onWheel = React.useCallback(
    (event: React.WheelEvent<HTMLDivElement>) => {
      if (!running || !inputEnabled) return;
      event.preventDefault();

      const vertical = Math.round((-event.deltaY / 100) * WHEEL_DELTA);
      const horizontal = Math.round((event.deltaX / 100) * WHEEL_DELTA);
      const mods = modifierKeys(event);

      if (vertical) {
        sendMouse(
          event.clientX,
          event.clientY,
          WM_MOUSEWHEEL,
          (((vertical & 0xffff) << 16) | mods) >>> 0,
        );
      }
      if (horizontal) {
        sendMouse(
          event.clientX,
          event.clientY,
          WM_MOUSEHWHEEL,
          (((horizontal & 0xffff) << 16) | mods) >>> 0,
        );
      }
    },
    [running, inputEnabled, sendMouse],
  );

  // Shift, control, alt and command reach the guest as their own key events, which is how a Win32
  // consumer is expected to track modifier state: only alt is in the message, as the lParam context bit
  // and the SYSKEY variant, exactly as the SDL backend sends it.
  const onKey = React.useCallback(
    (event: React.KeyboardEvent<HTMLDivElement>, down: boolean) => {
      if (!running || !inputEnabled) return;
      event.preventDefault();

      const vk = virtualKey(event.code);
      if (!vk) {
        setCounters(recordUnmappedKey);
        return;
      }

      const message = event.altKey
        ? down
          ? WM_SYSKEYDOWN
          : WM_SYSKEYUP
        : down
          ? WM_KEYDOWN
          : WM_KEYUP;
      const lParam =
        ((event.repeat ? 1 : 0) |
          (event.altKey ? 1 << 29 : 0) |
          (down ? 0 : 1 << 31)) >>>
        0;

      // No point to hit-test against, so the frontmost window stands in for keyboard focus.
      const focused = windowAt(null, desktopRef.current.windows);
      sendInput(focused, message, vk, lParam);

      if (down && event.key.length === 1) {
        sendInput(focused, WM_CHAR, event.key.codePointAt(0) || 0, lParam);
      }
    },
    [running, inputEnabled, sendInput],
  );

  const evicted = cache && cache.evictedBlocks > 0 ? cache.evictedBlocks : 0;
  const preparedTotal = cache ? cache.blocks + evicted : 0;
  const room = cache ? cache.quotaBytes - cache.usageBytes + cache.bytes : 0;
  const cannotHoldWhole = !!(
    cache &&
    cache.quotaBytes &&
    cache.attachedBytes &&
    cache.attachedBytes > room
  );

  const dotColor =
    !cache || !cache.enabled
      ? "bg-muted-foreground"
      : cache.bytes === 0
        ? "bg-muted-foreground"
        : evicted > 0 || cannotHoldWhole
          ? "bg-amber-500"
          : "bg-lime-600";

  return (
    <div className="flex h-full min-h-0 flex-col gap-2 p-2">
      <div
        ref={stageRef}
        tabIndex={0}
        onPointerDown={onPointerDown}
        onPointerUp={onPointerUp}
        onPointerMove={onPointerMove}
        onWheel={onWheel}
        onKeyDown={(e) => onKey(e, true)}
        onKeyUp={(e) => onKey(e, false)}
        onContextMenu={(e) => {
          if (running && inputEnabled) e.preventDefault();
        }}
        className={cn(
          "relative flex-1 min-h-0 overflow-auto rounded-md border bg-black/90 outline-none",
          running && inputEnabled && "cursor-crosshair",
        )}
      >
        {/* Always mounted, even before the first frame: drawFrame() needs a live canvas element to draw
            the very first frame into, and a canvas that only appears once a frame has already arrived
            can never receive one. It simply renders at 0x0 -- invisible -- until then. */}
        <div className="relative inline-block">
          <canvas
            ref={canvasRef}
            className="block max-w-full h-auto [image-rendering:pixelated]"
          />
          <div className="pointer-events-none absolute inset-0">
            {windows.map((w) => (
              <div
                key={w.id}
                className={cn(
                  "absolute border",
                  w.visible
                    ? "border-sky-400/85"
                    : "border-dashed border-muted-foreground/70",
                )}
                style={{
                  left: `${(w.x / desktopSize.width) * 100}%`,
                  top: `${(w.y / desktopSize.height) * 100}%`,
                  width: `${(w.w / desktopSize.width) * 100}%`,
                  height: `${(w.h / desktopSize.height) * 100}%`,
                }}
              >
                <span className="absolute -left-px -top-px bg-sky-400/85 px-1 text-[10px] text-black">
                  #{w.id}
                </span>
              </div>
            ))}
          </div>
        </div>

        {desktopSize.width === 0 && (
          <div className="flex h-full min-h-40 flex-col items-center justify-center gap-3 p-4 text-center text-sm text-muted-foreground">
            {running ? (
              <span>Waiting for the first frame&hellip;</span>
            ) : (
              <>
                <span>Run a demo to see the composed macOS desktop here.</span>
                <div className="flex flex-wrap justify-center gap-2">
                  {DEMOS.map((demo) => (
                    <Button
                      key={demo.path}
                      size="sm"
                      variant="secondary"
                      disabled={startingDemo !== null}
                      onClick={() => runDemo(demo.path, demo.needsRoot)}
                    >
                      {startingDemo === demo.path
                        ? "Starting\u2026"
                        : demo.label}
                    </Button>
                  ))}
                </div>
              </>
            )}
          </div>
        )}
      </div>

      {notice && (
        <div className="rounded-md border border-amber-600/50 bg-amber-500/10 px-2 py-1 text-xs text-amber-600">
          {notice}
        </div>
      )}

      <div className="flex flex-wrap items-center gap-3 text-xs text-muted-foreground">
        <label className="flex items-center gap-1.5">
          <input
            type="checkbox"
            checked={inputEnabled}
            onChange={(e) => setInputEnabled(e.target.checked)}
          />
          Send input
        </label>
        {COUNTER_ROWS.map((row) => {
          const value = row.value(counters, fps);
          if (value === null) return null;
          return (
            <span key={row.key}>
              {row.label}{" "}
              <b
                className={
                  row.warn(counters) ? "text-amber-600" : "text-foreground"
                }
              >
                {value}
              </b>
            </span>
          );
        })}
      </div>

      <div className="rounded-md border p-2 text-xs">
        <div className="mb-1 flex flex-wrap items-center gap-2">
          <span className={cn("inline-block h-2 w-2 rounded-full", dotColor)} />
          {!cache || !cache.enabled ? (
            <span className="text-muted-foreground">
              local root cache: unavailable in this browser
            </span>
          ) : cache.bypassed ? (
            <span className="text-muted-foreground">
              local root cache: off &mdash; every read goes straight to the
              network
            </span>
          ) : cache.bytes === 0 ? (
            <span className="text-muted-foreground">
              local root cache: empty &mdash; the root is read over the network
            </span>
          ) : (
            <span>
              local root cache: {bytes(cache.bytes)} in {cache.blocks} blocks
              {cache.attachedBytes
                ? ` of ${bytes(cache.attachedBytes)} attached`
                : ""}
            </span>
          )}
          <div className="ml-auto flex items-center gap-2">
            <label className="flex items-center gap-1.5">
              <input
                type="checkbox"
                checked={!!cache?.enabled && !cache?.bypassed}
                disabled={preparing || !cache?.enabled}
                onChange={(e) => handleToggleCache(e.target.checked)}
              />
              Cache the root locally
            </label>
            <Button
              size="sm"
              variant="secondary"
              disabled={preparing || !cache?.enabled || !!cache?.bypassed}
              onClick={handlePrepare}
            >
              Prepare root
            </Button>
            {preparing && (
              <Button
                size="sm"
                variant="outline"
                onClick={() => cancelPrepareRoot()}
              >
                Cancel
              </Button>
            )}
            <Button
              size="sm"
              variant="destructive"
              disabled={preparing || !cache?.enabled}
              onClick={handleClear}
            >
              Clear cache
            </Button>
          </div>
        </div>

        {prepareProgress && (
          <div className="mb-1 h-1 overflow-hidden rounded bg-muted">
            <div
              className="h-full bg-sky-500 transition-[width]"
              style={{
                width: `${prepareProgress.total ? Math.round((prepareProgress.done / prepareProgress.total) * 100) : 0}%`,
              }}
            />
          </div>
        )}

        {cache && (
          <div className="text-muted-foreground">
            {cache.quotaBytes
              ? `${bytes(cache.usageBytes)} used of ${bytes(cache.quotaBytes)} · `
              : ""}
            {cache.persistent
              ? "storage is persistent"
              : "storage is best-effort"}
          </div>
        )}

        {cacheError && <div className="mt-1 text-amber-600">{cacheError}</div>}

        {cache && !cache.persistent && cache.enabled && (
          <div className="mt-1 rounded border border-amber-600/50 bg-amber-500/10 px-2 py-1 text-amber-600">
            NOT PERSISTENT &mdash; the browser may delete this cache at any
            time, including part-way through a run. Add the page to your
            bookmarks or grant storage permission to keep it.
          </div>
        )}

        {evicted > 0 && (
          <div className="mt-1 rounded border border-amber-600/50 bg-amber-500/10 px-2 py-1 text-amber-600">
            {evicted} of {preparedTotal} prepared blocks (
            {bytes(evicted * 2097152)}) have been evicted &mdash; press Prepare
            root again.
          </div>
        )}

        {cannotHoldWhole && cache && (
          <div className="mt-1 rounded border border-amber-600/50 bg-amber-500/10 px-2 py-1 text-amber-600">
            the attached root is {bytes(cache.attachedBytes)} and only{" "}
            {bytes(room)} of quota is available, so it cannot be held whole.
          </div>
        )}
      </div>
    </div>
  );
}
