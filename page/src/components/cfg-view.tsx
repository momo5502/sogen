import React from "react";

import { Emulator } from "@/emulator";
import * as dbg from "@/debugger/api";

// Lightweight control-flow graph built incrementally from decoded
// instructions (reuses the debugger disassembly command). Self-contained SVG
// renderer with pan/zoom — no graph-library dependency, bounded work.

const MAX_INSNS = 400; // bound CFG generation for performance
const NODE_WIDTH = 260;
const LINE_HEIGHT = 14;
const NODE_PAD = 8;
const H_GAP = 60;
const V_GAP = 40;

// Never throw during render: a malformed address from the decode stream must
// not blank the whole UI.
function toBig(value: string | undefined | null): bigint | null {
  if (!value) {
    return null;
  }
  try {
    return BigInt(value);
  } catch {
    return null;
  }
}

interface BasicBlock {
  start: string; // hex address of first instruction
  insns: dbg.Instruction[];
  succ: string[]; // successor block start addresses (within window)
  // layout
  x: number;
  y: number;
  w: number;
  h: number;
}

function buildBlocks(insns: dbg.Instruction[]): Map<string, BasicBlock> {
  if (insns.length === 0) {
    return new Map();
  }

  const byAddr = new Map<string, dbg.Instruction>();
  for (const i of insns) {
    byAddr.set(i.address, i);
  }

  // Leaders: entry, branch targets, instructions after a branch/return.
  const leaders = new Set<string>([insns[0].address]);
  for (let k = 0; k < insns.length; k++) {
    const i = insns[k];
    if (i.isJump || i.isReturn) {
      const next = insns[k + 1];
      if (next) {
        leaders.add(next.address);
      }
      if (i.branch && byAddr.has(i.branch)) {
        leaders.add(i.branch);
      }
    }
  }

  const blocks = new Map<string, BasicBlock>();
  let current: BasicBlock | null = null;
  for (let k = 0; k < insns.length; k++) {
    const i = insns[k];
    if (leaders.has(i.address) || current === null) {
      current = {
        start: i.address,
        insns: [],
        succ: [],
        x: 0,
        y: 0,
        w: 0,
        h: 0,
      };
      blocks.set(i.address, current);
    }
    current.insns.push(i);

    const next = insns[k + 1];
    const isUncond = i.isJump && i.mnemonic === "jmp";
    if (i.isReturn) {
      current = null;
    } else if (i.isJump) {
      if (i.branch && blocks.has(i.branch) === false && byAddr.has(i.branch)) {
        current.succ.push(i.branch);
      } else if (i.branch && byAddr.has(i.branch)) {
        current.succ.push(i.branch);
      }
      if (!isUncond && next) {
        current.succ.push(next.address); // conditional fallthrough
      }
      current = null;
    } else if (next && leaders.has(next.address)) {
      current.succ.push(next.address);
      current = null;
    }
  }

  // Keep only successors that resolved to real blocks.
  for (const b of blocks.values()) {
    b.succ = [...new Set(b.succ)].filter((s) => blocks.has(s));
  }
  return blocks;
}

// Simple layered layout: BFS depth -> row, sequential -> column.
function layout(blocks: Map<string, BasicBlock>, entry: string) {
  const depth = new Map<string, number>();
  const queue: string[] = [];
  if (blocks.has(entry)) {
    depth.set(entry, 0);
    queue.push(entry);
  }
  while (queue.length > 0) {
    const id = queue.shift()!;
    const d = depth.get(id)!;
    for (const s of blocks.get(id)!.succ) {
      if (!depth.has(s)) {
        depth.set(s, d + 1);
        queue.push(s);
      }
    }
  }
  let orphanDepth = 0;
  for (const id of blocks.keys()) {
    if (!depth.has(id)) {
      depth.set(id, orphanDepth++);
    }
  }

  const rows = new Map<number, string[]>();
  for (const [id, d] of depth) {
    if (!rows.has(d)) {
      rows.set(d, []);
    }
    rows.get(d)!.push(id);
  }

  for (const [d, ids] of rows) {
    ids.forEach((id, col) => {
      const b = blocks.get(id)!;
      b.h = NODE_PAD * 2 + b.insns.length * LINE_HEIGHT;
      b.w = NODE_WIDTH;
      b.x = col * (NODE_WIDTH + H_GAP);
      b.y = d * 1; // temp; resolved below using per-row max height
    });
  }

  // Resolve y per depth row using cumulative max heights.
  const sortedDepths = [...rows.keys()].sort((a, b) => a - b);
  let yCursor = 0;
  for (const d of sortedDepths) {
    let rowH = 0;
    for (const id of rows.get(d)!) {
      const b = blocks.get(id)!;
      b.y = yCursor;
      rowH = Math.max(rowH, b.h);
    }
    yCursor += rowH + V_GAP;
  }
}

export interface CfgViewProps {
  emulator: Emulator;
  paused: boolean;
  entry: bigint;
  rip: bigint;
  generation: number;
  onSelect: (address: bigint) => void;
}

export function CfgView({
  emulator,
  paused,
  entry,
  rip,
  generation,
  onSelect,
}: CfgViewProps) {
  const [blocks, setBlocks] = React.useState<BasicBlock[]>([]);
  const [view, setView] = React.useState({ x: 20, y: 20, scale: 1 });
  const panning = React.useRef<{ x: number; y: number } | null>(null);

  React.useEffect(() => {
    if (!paused || entry === BigInt(0)) {
      return;
    }
    let cancelled = false;
    dbg.disassemble(emulator, entry, MAX_INSNS).then((insns) => {
      if (cancelled) {
        return;
      }
      const map = buildBlocks(insns);
      const entryAddr = insns.length > 0 ? insns[0].address : "";
      const ripAddr = rip.toString(16);
      setBlocks((prev) => {
        const keepExisting = prev.some((b) =>
          b.insns.some((i) => toBig(i.address)?.toString(16) === ripAddr),
        );
        if (keepExisting) {
          return prev;
        }

        const next = new Map<string, BasicBlock>();
        for (const block of map.values()) {
          next.set(block.start, block);
        }
        layout(next, entryAddr);
        return [...next.values()];
      });
    });
    return () => {
      cancelled = true;
    };
  }, [emulator, paused, entry, generation, rip]);

  const ripKey = rip.toString(16);
  const byStart = React.useMemo(() => {
    const m = new Map<string, BasicBlock>();
    for (const b of blocks) {
      m.set(b.start, b);
    }
    return m;
  }, [blocks]);

  const currentInsnKey = ripKey;
  const containsRip = (b: BasicBlock) =>
    b.insns.some((i) => toBig(i.address)?.toString(16) === currentInsnKey);

  if (blocks.length === 0) {
    return (
      <div className="p-3 text-xs text-muted-foreground">
        {paused ? "No control flow to graph." : "Pause to graph."}
      </div>
    );
  }

  return (
    <div
      className="h-full w-full overflow-hidden bg-black/20"
      onWheel={(e) => {
        const factor = e.deltaY < 0 ? 1.1 : 0.9;
        setView((v) => ({
          ...v,
          scale: Math.min(2.5, Math.max(0.2, v.scale * factor)),
        }));
      }}
      onMouseDown={(e) => {
        panning.current = { x: e.clientX - view.x, y: e.clientY - view.y };
      }}
      onMouseMove={(e) => {
        const pan = panning.current;
        if (pan) {
          setView((v) => ({
            ...v,
            x: e.clientX - pan.x,
            y: e.clientY - pan.y,
          }));
        }
      }}
      onMouseUp={() => {
        panning.current = null;
      }}
      onMouseLeave={() => {
        panning.current = null;
      }}
    >
      <svg className="h-full w-full select-none">
        <g transform={`translate(${view.x},${view.y}) scale(${view.scale})`}>
          {blocks.flatMap((b) =>
            b.succ.map((s) => {
              const t = byStart.get(s);
              if (!t) {
                return null;
              }
              const x1 = b.x + b.w / 2;
              const y1 = b.y + b.h;
              const x2 = t.x + t.w / 2;
              const y2 = t.y;
              return (
                <path
                  key={`${b.start}-${s}`}
                  d={`M${x1},${y1} C${x1},${y1 + 30} ${x2},${y2 - 30} ${x2},${y2}`}
                  className="stroke-muted-foreground/50"
                  fill="none"
                  strokeWidth={1}
                />
              );
            }),
          )}

          {blocks.map((b) => {
            const active = containsRip(b);
            return (
              <g
                key={b.start}
                transform={`translate(${b.x},${b.y})`}
                onClick={() => {
                  const a = toBig(b.start);
                  if (a !== null) {
                    onSelect(a);
                  }
                }}
                className="cursor-pointer"
              >
                <rect
                  width={b.w}
                  height={b.h}
                  rx={4}
                  className={
                    active
                      ? "fill-primary/30 stroke-primary shadow-[0_0_0_2px_rgba(59,130,246,0.45)]"
                      : "fill-card stroke-border"
                  }
                  strokeWidth={active ? 2 : 1}
                />
                {b.insns.map((i, idx) => {
                  const isCurrent = toBig(i.address)?.toString(16) === ripKey;
                  const lineY = NODE_PAD + (idx + 1) * LINE_HEIGHT - 3;
                  return (
                    <React.Fragment key={i.address}>
                      {isCurrent && (
                        <rect
                          x={NODE_PAD - 4}
                          y={lineY - LINE_HEIGHT + 3}
                          width={b.w - NODE_PAD * 2 + 8}
                          height={LINE_HEIGHT + 1}
                          rx={2}
                          className="fill-primary/40"
                        />
                      )}
                      <text
                        x={NODE_PAD}
                        y={lineY}
                        className={
                          "fill-foreground font-mono " +
                          (isCurrent ? "font-bold" : "")
                        }
                        fontSize={10}
                      >
                        {isCurrent ? "▶ " : "  "}
                        {i.address.slice(2).padStart(8, "0")} {i.mnemonic}{" "}
                        {i.operands.slice(0, 22)}
                      </text>
                    </React.Fragment>
                  );
                })}
              </g>
            );
          })}
        </g>
      </svg>
    </div>
  );
}
