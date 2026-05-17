import React from "react";

import { List, useListRef, type RowComponentProps } from "react-window";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";

import { ArrowRightShort, XLg } from "react-bootstrap-icons";

import { Emulator, MemoryRegion } from "@/emulator";

// 16 bytes per hex row, fetched from the backend in fixed-size chunks so large
// regions are lazily streamed instead of loaded all at once.
const ROW_BYTES = 16;
const CHUNK_BYTES = 1024;
const ROW_HEIGHT = 22;
const FALLBACK_VIEW_SIZE = 0x1000;
// Hex window size slider: powers of two from 256 B to 16 MiB.
const MIN_WINDOW_EXP = 8;
const MAX_WINDOW_EXP = 24;
// Cap the rendered window so huge free/reserved ranges don't overflow the
// virtual list (rowCount * rowHeight must stay within CSS limits). Navigate
// large regions via the address jump or the size slider.
const MAX_VIEW_SIZE = Math.pow(2, MAX_WINDOW_EXP);

function clampViewSize(size: number): number {
  return Math.min(Math.max(ROW_BYTES, size), MAX_VIEW_SIZE);
}

export interface HexViewerSelection {
  // Byte offsets relative to the current view base. `anchor` is where the
  // selection started, `head` is the latest dragged-to byte (inclusive).
  anchor: number;
  head: number;
}

type ChunkState = Uint8Array | "pending" | "error";
type ChunkMap = Map<string, ChunkState>;

function parseAddress(value: string): bigint | null {
  const trimmed = value
    .trim()
    .replace(/^0x/i, "")
    .replace(/[`_\s]/g, "");
  if (trimmed.length === 0 || !/^[0-9a-fA-F]+$/.test(trimmed)) {
    return null;
  }
  try {
    return BigInt("0x" + trimmed);
  } catch {
    return null;
  }
}

function formatAddress(address: bigint): string {
  return "0x" + address.toString(16).padStart(12, "0");
}

function formatSize(size: number): string {
  if (size < 1024) {
    return `${size} B`;
  }
  const units = ["KB", "MB", "GB", "TB"];
  let value = size / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value.toFixed(value < 10 ? 2 : 0)} ${units[unit]}`;
}

function chunkBaseOf(address: bigint): bigint {
  return address - (address % BigInt(CHUNK_BYTES));
}

function selectionRange(
  selection: HexViewerSelection | null,
): [number, number] | null {
  if (!selection) {
    return null;
  }
  return [
    Math.min(selection.anchor, selection.head),
    Math.max(selection.anchor, selection.head),
  ];
}

/* -------------------------------------------------------------------------- */
/*                                AddressInput                                */
/* -------------------------------------------------------------------------- */

export interface AddressInputProps {
  onJump: (address: bigint) => void;
  disabled?: boolean;
}

export function AddressInput({ onJump, disabled }: AddressInputProps) {
  const [value, setValue] = React.useState("");
  const [invalid, setInvalid] = React.useState(false);

  const submit = () => {
    const address = parseAddress(value);
    if (address === null) {
      setInvalid(true);
      return;
    }
    setInvalid(false);
    onJump(address);
  };

  return (
    <form
      className="flex items-center gap-1.5"
      onSubmit={(e) => {
        e.preventDefault();
        submit();
      }}
    >
      <Input
        value={value}
        disabled={disabled}
        aria-invalid={invalid}
        spellCheck={false}
        placeholder="Jump to address (hex)"
        className="h-8 font-mono text-xs"
        onChange={(e) => {
          setValue(e.target.value);
          if (invalid) {
            setInvalid(false);
          }
        }}
      />
      <Button
        type="submit"
        size="sm"
        variant="secondary"
        className="fancy h-8"
        disabled={disabled}
        title="Go to address"
      >
        <ArrowRightShort />
      </Button>
    </form>
  );
}

/* -------------------------------------------------------------------------- */
/*                             MemoryRegionsPanel                             */
/* -------------------------------------------------------------------------- */

export interface MemoryRegionsPanelProps {
  regions: MemoryRegion[] | null | undefined;
  selected: MemoryRegion | null;
  onSelect: (region: MemoryRegion) => void;
}

export function MemoryRegionsPanel({
  regions,
  selected,
  onSelect,
}: MemoryRegionsPanelProps) {
  if (regions === undefined) {
    return (
      <div className="p-3 text-xs text-muted-foreground">Loading regions…</div>
    );
  }

  if (regions === null) {
    return (
      <div className="p-3 text-xs text-muted-foreground">
        Region enumeration is not supported by this emulator build. You can
        still inspect memory using the address jump above.
      </div>
    );
  }

  if (regions.length === 0) {
    return (
      <div className="p-3 text-xs text-muted-foreground">
        No memory regions.
      </div>
    );
  }

  return (
    <div className="flex flex-col text-xs font-mono">
      {regions.map((region, index) => {
        const isSelected =
          selected !== null &&
          selected.base === region.base &&
          selected.state === region.state;
        return (
          <button
            key={`${region.base}-${region.state}-${index}`}
            onClick={() => onSelect(region)}
            className={cn(
              "flex flex-col gap-0.5 border-b border-border/50 px-3 py-2 text-left transition-colors hover:bg-accent/50",
              isSelected && "bg-accent",
            )}
          >
            <div className="flex items-center justify-between gap-2">
              <span className="font-medium">
                {formatAddress(BigInt(region.base))}
              </span>
              <span className="text-muted-foreground">
                {formatSize(region.size)}
              </span>
            </div>
            <div className="flex items-center justify-between gap-2 text-[10px] text-muted-foreground">
              <span>
                <span
                  className={cn(
                    region.state === "commit"
                      ? "text-emerald-500"
                      : "text-amber-500",
                  )}
                >
                  {region.protection || "---"}
                </span>{" "}
                · {region.kind} · {region.state}
              </span>
              {region.module && (
                <span className="truncate max-w-[45%]" title={region.module}>
                  {region.module}
                </span>
              )}
            </div>
          </button>
        );
      })}
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/*                                 HexViewer                                  */
/* -------------------------------------------------------------------------- */

interface HexRowOwnProps {
  viewBase: bigint;
  getByte: (absolute: bigint) => number | null;
  selection: HexViewerSelection | null;
  onByteDown: (offset: number) => void;
  onByteEnter: (offset: number) => void;
}

function HexRow({
  index,
  style,
  viewBase,
  getByte,
  selection,
  onByteDown,
  onByteEnter,
}: RowComponentProps<HexRowOwnProps>) {
  const rowOffset = index * ROW_BYTES;
  const rowAddress = viewBase + BigInt(rowOffset);
  const range = selectionRange(selection);

  const hexCells: React.ReactNode[] = [];
  const asciiCells: React.ReactNode[] = [];

  for (let i = 0; i < ROW_BYTES; i++) {
    const offset = rowOffset + i;
    const byte = getByte(rowAddress + BigInt(i));
    const isSelected =
      range !== null && offset >= range[0] && offset <= range[1];

    const hexText =
      byte === null ? "??" : byte.toString(16).padStart(2, "0").toUpperCase();
    const asciiChar =
      byte === null
        ? " "
        : byte >= 0x20 && byte < 0x7f
          ? String.fromCharCode(byte)
          : ".";

    hexCells.push(
      <span
        key={`h${i}`}
        onMouseDown={(e) => {
          e.preventDefault();
          onByteDown(offset);
        }}
        onMouseEnter={() => onByteEnter(offset)}
        className={cn(
          "inline-block w-[1.4rem] cursor-text text-center select-none",
          i === 8 && "ml-2",
          isSelected && "bg-primary text-primary-foreground rounded-xs",
          byte === null && "text-muted-foreground/40",
        )}
      >
        {hexText}
      </span>,
    );

    asciiCells.push(
      <span
        key={`a${i}`}
        onMouseDown={(e) => {
          e.preventDefault();
          onByteDown(offset);
        }}
        onMouseEnter={() => onByteEnter(offset)}
        className={cn(
          "inline-block w-[0.6rem] cursor-text text-center select-none",
          isSelected && "bg-primary text-primary-foreground rounded-xs",
          byte === null && "text-muted-foreground/40",
        )}
      >
        {asciiChar}
      </span>,
    );
  }

  return (
    <div
      style={style}
      className="flex items-center whitespace-pre px-3 text-xs font-mono leading-none"
    >
      <span className="mr-4 text-muted-foreground select-none">
        {formatAddress(rowAddress)}
      </span>
      <span className="mr-4">{hexCells}</span>
      <span className="text-muted-foreground">{asciiCells}</span>
    </div>
  );
}

export interface HexViewerProps {
  emulator: Emulator;
  viewBase: bigint;
  viewSize: number;
  paused: boolean;
  scrollToOffset: number;
}

// `key` is set by the parent to `generation|base|size`, so a new snapshot,
// region or size change remounts this component and discards the chunk cache.
export function HexViewer({
  emulator,
  viewBase,
  viewSize,
  paused,
  scrollToOffset,
}: HexViewerProps) {
  const listRef = useListRef(null);
  const [chunks, setChunks] = React.useState<ChunkMap>(() => new Map());
  const [selection, setSelection] = React.useState<HexViewerSelection | null>(
    null,
  );
  const selecting = React.useRef(false);

  const rowCount = Math.max(1, Math.ceil(viewSize / ROW_BYTES));

  React.useEffect(() => {
    const stop = () => {
      selecting.current = false;
    };
    window.addEventListener("mouseup", stop);
    return () => window.removeEventListener("mouseup", stop);
  }, []);

  React.useEffect(() => {
    listRef.current?.scrollToRow({
      index: Math.floor(scrollToOffset / ROW_BYTES),
      align: "start",
      behavior: "auto",
    });
  }, [scrollToOffset, listRef]);

  const ensureChunk = React.useCallback(
    (chunkBase: bigint) => {
      if (!paused) {
        return;
      }
      const key = chunkBase.toString(16);
      setChunks((prev) => {
        if (prev.has(key)) {
          return prev;
        }
        const next = new Map(prev);
        next.set(key, "pending");
        emulator.readMemory(chunkBase, CHUNK_BYTES).then((data) => {
          setChunks((current) => {
            const updated = new Map(current);
            updated.set(key, data ?? "error");
            return updated;
          });
        });
        return next;
      });
    },
    [emulator, paused],
  );

  const getByte = React.useCallback(
    (absolute: bigint): number | null => {
      const chunkBase = chunkBaseOf(absolute);
      const entry = chunks.get(chunkBase.toString(16));
      if (!entry || entry === "pending" || entry === "error") {
        return null;
      }
      const idx = Number(absolute - chunkBase);
      return idx < entry.length ? entry[idx] : null;
    },
    [chunks],
  );

  const handleRowsRendered = React.useCallback(
    (visible: { startIndex: number; stopIndex: number }) => {
      const startAddr = viewBase + BigInt(visible.startIndex * ROW_BYTES);
      const stopAddr = viewBase + BigInt((visible.stopIndex + 1) * ROW_BYTES);
      for (
        let chunk = chunkBaseOf(startAddr);
        chunk < stopAddr;
        chunk += BigInt(CHUNK_BYTES)
      ) {
        ensureChunk(chunk);
      }
    },
    [viewBase, ensureChunk],
  );

  const onByteDown = React.useCallback((offset: number) => {
    selecting.current = true;
    setSelection({ anchor: offset, head: offset });
  }, []);

  const onByteEnter = React.useCallback((offset: number) => {
    if (!selecting.current) {
      return;
    }
    setSelection((prev) =>
      prev ? { anchor: prev.anchor, head: offset } : null,
    );
  }, []);

  const range = selectionRange(selection);
  let selectionInfo = "No selection";
  if (range) {
    const length = range[1] - range[0] + 1;
    const start = viewBase + BigInt(range[0]);
    const bytes: string[] = [];
    for (let i = range[0]; i <= range[1] && bytes.length < 16; i++) {
      const b = getByte(viewBase + BigInt(i));
      bytes.push(b === null ? "??" : b.toString(16).padStart(2, "0"));
    }
    selectionInfo =
      `${formatAddress(start)} · ${length} byte${length === 1 ? "" : "s"} · ` +
      bytes.join(" ") +
      (length > 16 ? " …" : "");
  }

  return (
    <div className="flex flex-1 flex-col min-h-0">
      <div className="flex-1 min-h-0">
        <List
          listRef={listRef}
          rowCount={rowCount}
          rowHeight={ROW_HEIGHT}
          overscanCount={8}
          className="h-full"
          style={{ height: "100%" }}
          onRowsRendered={handleRowsRendered}
          rowComponent={HexRow}
          rowProps={{
            viewBase,
            getByte,
            selection,
            onByteDown,
            onByteEnter,
          }}
        />
      </div>
      <div className="border-t border-border/50 px-3 py-1.5 text-[11px] font-mono text-muted-foreground truncate">
        {selectionInfo}
      </div>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/*                                 MemoryView                                 */
/* -------------------------------------------------------------------------- */

export interface MemoryViewProps {
  emulator: Emulator;
  paused: boolean;
  onClose: () => void;
}

interface MemoryViewState {
  base: bigint;
  size: number;
  scrollToOffset: number;
}

export function MemoryView({ emulator, paused, onClose }: MemoryViewProps) {
  const [regions, setRegions] = React.useState<MemoryRegion[] | null>(null);
  const [regionsGen, setRegionsGen] = React.useState(-1);
  const [selectedRegion, setSelectedRegion] =
    React.useState<MemoryRegion | null>(null);
  const [view, setView] = React.useState<MemoryViewState>({
    base: BigInt(0),
    size: FALLBACK_VIEW_SIZE,
    scrollToOffset: 0,
  });
  const [generation, setGeneration] = React.useState(0);
  const wasPaused = React.useRef(false);

  // Resizable docked width (drag the left edge).
  const [width, setWidth] = React.useState(() =>
    Math.min(760, Math.round(window.innerWidth * 0.6)),
  );
  const dragging = React.useRef(false);

  React.useEffect(() => {
    const onMove = (e: MouseEvent) => {
      if (!dragging.current) {
        return;
      }
      const next = window.innerWidth - e.clientX;
      setWidth(Math.min(Math.max(next, 360), window.innerWidth - 160));
    };
    const onUp = () => {
      if (dragging.current) {
        dragging.current = false;
        document.body.style.userSelect = "";
      }
    };
    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onUp);
    return () => {
      window.removeEventListener("mousemove", onMove);
      window.removeEventListener("mouseup", onUp);
    };
  }, []);

  // Each fresh entry into the paused state is a new snapshot of process
  // memory: bump `generation` so regions are refetched and the hex cache
  // (keyed by generation) is dropped.
  React.useEffect(() => {
    if (paused && !wasPaused.current) {
      setGeneration((g) => g + 1);
    }
    wasPaused.current = paused;
  }, [paused]);

  const selectRegion = React.useCallback((region: MemoryRegion) => {
    setSelectedRegion(region);
    setView({
      base: BigInt(region.base),
      size: clampViewSize(region.size),
      scrollToOffset: 0,
    });
  }, []);

  React.useEffect(() => {
    if (!paused) {
      return;
    }

    let cancelled = false;
    emulator.getMemoryRegions().then((result) => {
      if (cancelled) {
        return;
      }
      setRegions(result);
      setRegionsGen(generation);

      if (result && result.length > 0) {
        const initial =
          result.find((r) => r.state === "commit" && r.module) ??
          result.find((r) => r.state === "commit") ??
          result[0];
        selectRegion(initial);
      }
    });

    return () => {
      cancelled = true;
    };
  }, [emulator, paused, generation, selectRegion]);

  const jumpTo = React.useCallback(
    (address: bigint) => {
      const containing =
        regions?.find((r) => {
          const base = BigInt(r.base);
          return (
            r.state === "commit" &&
            address >= base &&
            address < base + BigInt(r.size)
          );
        }) ??
        regions?.find((r) => {
          const base = BigInt(r.base);
          return address >= base && address < base + BigInt(r.size);
        });

      if (containing) {
        const regionBase = BigInt(containing.base);
        setSelectedRegion(containing);
        if (containing.size <= MAX_VIEW_SIZE) {
          setView({
            base: regionBase,
            size: clampViewSize(containing.size),
            scrollToOffset: Number(address - regionBase),
          });
        } else {
          // Region too large to render whole: open a capped window at the
          // target address (still aligned within the region).
          const aligned = address - (address % BigInt(ROW_BYTES));
          setView({
            base: aligned,
            size: MAX_VIEW_SIZE,
            scrollToOffset: 0,
          });
        }
      } else {
        // No known region: open an ad-hoc window around the address.
        const aligned = address - (address % BigInt(ROW_BYTES));
        setSelectedRegion(null);
        setView({ base: aligned, size: FALLBACK_VIEW_SIZE, scrollToOffset: 0 });
      }
    },
    [regions],
  );

  const resizeWindow = React.useCallback((exp: number) => {
    setView((v) => ({
      base: v.base,
      size: Math.pow(2, exp),
      scrollToOffset: 0,
    }));
  }, []);

  const sizeExp = Math.min(
    MAX_WINDOW_EXP,
    Math.max(MIN_WINDOW_EXP, Math.round(Math.log2(view.size))),
  );

  const loading = regionsGen !== generation;

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
      <div className="flex items-center justify-between gap-2 border-b p-2">
        <span className="text-sm font-medium">Memory View</span>
        <div className="flex-1" />
        <div className="hidden items-center gap-2 sm:flex">
          <span className="text-[11px] whitespace-nowrap text-muted-foreground">
            Window
          </span>
          <input
            type="range"
            min={MIN_WINDOW_EXP}
            max={MAX_WINDOW_EXP}
            step={1}
            value={sizeExp}
            disabled={!paused}
            title="Hex window size"
            onChange={(e) => resizeWindow(parseInt(e.target.value, 10))}
            className="h-1 w-28 cursor-pointer accent-primary disabled:opacity-50"
          />
          <span className="w-14 text-right font-mono text-[11px] text-muted-foreground">
            {formatSize(view.size)}
          </span>
        </div>
        <AddressInput onJump={jumpTo} disabled={!paused} />
        <Button
          size="sm"
          variant="secondary"
          className="fancy h-8"
          title="Close Memory View"
          onClick={onClose}
        >
          <XLg />
        </Button>
      </div>

      <div className="relative flex flex-1 min-h-0">
        <div className="w-[260px] shrink-0 overflow-y-auto border-r">
          <MemoryRegionsPanel
            regions={loading ? undefined : regions}
            selected={selectedRegion}
            onSelect={selectRegion}
          />
        </div>

        <div className="flex flex-1 flex-col min-h-0">
          <HexViewer
            key={`${generation}|${view.base}|${view.size}`}
            emulator={emulator}
            viewBase={view.base}
            viewSize={view.size}
            paused={paused}
            scrollToOffset={view.scrollToOffset}
          />
        </div>

        {!paused && (
          <div className="absolute inset-0 flex items-center justify-center bg-background/80 text-sm text-muted-foreground backdrop-blur-xs">
            Pause emulation to inspect memory.
          </div>
        )}
      </div>
    </div>
  );
}
