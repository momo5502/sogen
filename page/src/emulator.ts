import { Settings, translateSettings } from "./settings";

import * as flatbuffers from "flatbuffers";
import * as fbDebugger from "@/fb/debugger";

type LogHandler = (lines: string[]) => void;

export enum EmulationState {
  Stopped,
  Paused,
  Running,
  Success,
  Failed,
}

export interface EmulationStatus {
  activeThreads: number;
  reservedMemory: BigInt;
  committedMemory: BigInt;
  executedInstructions: BigInt;
}

/**
 * A single virtual memory region as reported by the paused emulator backend.
 * `base` is a hex string (e.g. "0x140000000") so 64-bit values survive JSON.
 */
export interface MemoryRegion {
  base: string;
  size: number;
  protection: string;
  state: "reserve" | "commit";
  kind: string;
  module: string | null;
}

/** Result of a read-only memory read against a paused snapshot. */
export interface MemoryReadResult {
  address: bigint;
  data: Uint8Array;
}

// One outstanding read. The entry stays in its per-address queue even after it
// settles (timeout) so the strict 1:1 positional mapping between sequential
// backend responses and requests is preserved (a timed-out request becomes a
// tombstone that absorbs its own late response instead of stealing another's).
interface PendingRead {
  settle: (result: Uint8Array | null) => void;
  settled: boolean;
}

type RegionsResolver = (regions: MemoryRegion[] | null) => void;

const MEMORY_READ_TIMEOUT_MS = 8000;
const MEMORY_REGIONS_TIMEOUT_MS = 4000;

function createDefaultEmulationStatus(): EmulationStatus {
  return {
    executedInstructions: BigInt(0),
    activeThreads: 0,
    reservedMemory: BigInt(0),
    committedMemory: BigInt(0),
  };
}

export function isFinalState(state: EmulationState) {
  switch (state) {
    case EmulationState.Stopped:
    case EmulationState.Success:
    case EmulationState.Failed:
      return true;

    default:
      return false;
  }
}

function base64Encode(uint8Array: Uint8Array): string {
  let binaryString = "";
  for (let i = 0; i < uint8Array.byteLength; i++) {
    binaryString += String.fromCharCode(uint8Array[i]);
  }

  return btoa(binaryString);
}

function base64Decode(data: string) {
  const binaryString = atob(data);

  const len = binaryString.length;
  const bytes = new Uint8Array(len);

  for (let i = 0; i < len; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }

  return bytes;
}

function decodeEvent(data: string) {
  const array = base64Decode(data);
  const buffer = new flatbuffers.ByteBuffer(array);
  const event = fbDebugger.DebugEvent.getRootAsDebugEvent(buffer);
  return event.unpack();
}

type StateChangeHandler = (state: EmulationState) => void;
type StatusUpdateHandler = (status: EmulationStatus) => void;

const cacheBuster = undefined; //import.meta.env.VITE_BUILD_TIME || Date.now();

export class Emulator {
  logHandler: LogHandler;
  stateChangeHandler: StateChangeHandler;
  stautsUpdateHandler: StatusUpdateHandler;
  terminatePromise: Promise<number | null>;
  terminateResolve: (value: number | null) => void;
  terminateReject: (reason?: any) => void;
  worker: Worker;
  state: EmulationState = EmulationState.Stopped;
  exit_status: number | null = null;
  start_time: Date = new Date();
  pause_time: Date | null = null;
  paused_time: number = 0;

  // Outstanding read-only memory requests, keyed by requested address (hex).
  // The paused backend answers in FIFO order per address.
  private pendingMemoryReads: Map<string, PendingRead[]> = new Map();
  private pendingRegionRequests: RegionsResolver[] = [];

  constructor(
    logHandler: LogHandler,
    stateChangeHandler: StateChangeHandler,
    stautsUpdateHandler: StatusUpdateHandler,
  ) {
    this.logHandler = logHandler;
    this.stateChangeHandler = stateChangeHandler;
    this.stautsUpdateHandler = stautsUpdateHandler;
    this.terminateResolve = () => {};
    this.terminateReject = () => {};
    this.terminatePromise = new Promise((resolve, reject) => {
      this.terminateResolve = resolve;
      this.terminateReject = reject;
    });

    const busterParams = cacheBuster ? `?${cacheBuster}` : "";

    this.worker = new Worker("./emulator-worker.js" + busterParams);
    this.worker.onerror = this._onError.bind(this);
    this.worker.onmessage = (e) => queueMicrotask(() => this._onMessage(e));
  }

  async start(settings: Settings, file: string) {
    this.start_time = new Date();
    this.pause_time = null;
    this.paused_time = 0;
    this._setState(EmulationState.Running);
    this.stautsUpdateHandler(createDefaultEmulationStatus());

    const options = translateSettings(settings);

    this.worker.postMessage({
      message: "run",
      data: {
        file,
        options: options.emulatorOptions,
        arguments: options.applicationOptions,
        persist: settings.persist,
        wasm64: settings.wasm64,
        cacheBuster,
      },
    });
  }

  updateState() {
    this.sendEvent(
      new fbDebugger.DebugEventT(
        fbDebugger.Event.GetStateRequest,
        new fbDebugger.GetStateRequestT(),
      ),
    );
  }

  getState() {
    return this.state;
  }

  stop() {
    this.worker.terminate();
    this._flushPendingMemoryRequests();
    this._setState(EmulationState.Stopped);
    this.terminateResolve(null);
  }

  onTerminate() {
    return this.terminatePromise;
  }

  sendEvent(event: fbDebugger.DebugEventT) {
    const builder = new flatbuffers.Builder(1024);
    fbDebugger.DebugEvent.finishDebugEventBuffer(builder, event.pack(builder));

    const message = base64Encode(builder.asUint8Array());

    this.worker.postMessage({
      message: "event",
      data: message,
    });
  }

  pause() {
    this.sendEvent(
      new fbDebugger.DebugEventT(
        fbDebugger.Event.PauseRequest,
        new fbDebugger.PauseRequestT(),
      ),
    );

    this.updateState();
  }

  resume() {
    this.sendEvent(
      new fbDebugger.DebugEventT(
        fbDebugger.Event.RunRequest,
        new fbDebugger.RunRequestT(),
      ),
    );

    this.updateState();
  }

  /**
   * Read-only read of `size` bytes at `address` from the paused snapshot.
   * Resolves `null` if the range is unmapped, the read fails, or the backend
   * does not answer (e.g. emulation is not paused). Never mutates emulator
   * state.
   */
  readMemory(address: bigint, size: number): Promise<Uint8Array | null> {
    if (size <= 0 || this.state !== EmulationState.Paused) {
      return Promise.resolve(null);
    }

    const key = address.toString(16);

    return new Promise<Uint8Array | null>((resolve) => {
      const entry: PendingRead = { settled: false, settle: () => {} };

      // On timeout we do NOT remove the entry from the queue; it is left as a
      // tombstone so later responses still line up positionally with their
      // originating requests.
      const finish = (value: Uint8Array | null) => {
        if (entry.settled) {
          return;
        }
        entry.settled = true;
        clearTimeout(timer);
        resolve(value);
      };
      entry.settle = finish;

      const timer = setTimeout(() => finish(null), MEMORY_READ_TIMEOUT_MS);

      const queue = this.pendingMemoryReads.get(key);
      if (queue) {
        queue.push(entry);
      } else {
        this.pendingMemoryReads.set(key, [entry]);
      }

      this.sendEvent(
        new fbDebugger.DebugEventT(
          fbDebugger.Event.ReadMemoryRequest,
          new fbDebugger.ReadMemoryRequestT(address, size),
        ),
      );
    });
  }

  /**
   * Enumerate the emulated virtual memory layout from the paused snapshot.
   *
   * Prefers the native backend enumeration (accurate protection flags and
   * module names). If the running emulator build does not implement it, falls
   * back to probing the address space with read-only reads so the feature
   * still works (approximate ranges, no protection/module metadata).
   * Resolves `null` only when emulation is not paused.
   */
  async getMemoryRegions(): Promise<MemoryRegion[] | null> {
    if (this.state !== EmulationState.Paused) {
      return null;
    }

    const native = await this._requestMemoryRegions();
    if (native) {
      return native;
    }

    return this._probeMemoryRegions();
  }

  private _requestMemoryRegions(): Promise<MemoryRegion[] | null> {
    if (this.state !== EmulationState.Paused) {
      return Promise.resolve(null);
    }

    return new Promise<MemoryRegion[] | null>((resolve) => {
      let settled = false;
      const finish = (value: MemoryRegion[] | null) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);

        const idx = this.pendingRegionRequests.indexOf(resolver);
        if (idx >= 0) {
          this.pendingRegionRequests.splice(idx, 1);
        }

        resolve(value);
      };

      const resolver: RegionsResolver = (regions) => finish(regions);
      const timer = setTimeout(() => finish(null), MEMORY_REGIONS_TIMEOUT_MS);

      this.pendingRegionRequests.push(resolver);

      this.sendEvent(
        new fbDebugger.DebugEventT(
          fbDebugger.Event.GetMemoryRegionsRequest,
          new fbDebugger.GetMemoryRegionsRequestT(),
        ),
      );
    });
  }

  /**
   * Fallback used when the backend has no region-enumeration support: discover
   * mapped memory with read-only 1-byte reads.
   *
   * Unmapped gaps are scanned *exhaustively* at allocation granularity (no
   * exponential stride that could jump over a whole region, and no early abort
   * that could return an empty list). Well-known base addresses are probed
   * first so the main image/heap is found even when it sits far above the
   * scan start and the read budget would otherwise be exhausted by the gap.
   * Bounded by a read budget and a wall-clock deadline so it never hangs the
   * UI; returns whatever was found so far when the budget runs out.
   */
  private async _probeMemoryRegions(): Promise<MemoryRegion[]> {
    const GRAN = BigInt(0x10000); // allocation granularity (64 KiB)
    const ALIGN_MASK = ~(GRAN - BigInt(1));
    const MIN = BigInt(0x10000);
    const MAX = BigInt("0x7fffffffffff");
    const END_STEP_CAP = GRAN * BigInt(64); // cap region-end stride at 4 MiB
    const MAX_READS = 8000;
    const deadline = Date.now() + 20000;

    // Likely allocation bases (low 32-bit space, 64-bit defaults, common PE
    // image bases) so a far-away first mapping is still discovered.
    const SEEDS = [
      0x10000, 0x40000, 0x400000, 0x10000000, 0x70000000, 0x100000000,
      0x140000000, 0x180000000, 0x200000000, 0x1000000000,
    ];

    const regions: MemoryRegion[] = [];
    const recorded: { start: bigint; end: bigint }[] = [];
    let reads = 0;

    const readable = async (addr: bigint): Promise<boolean> => {
      reads++;
      return (await this.readMemory(addr, 1)) !== null;
    };
    const budgetLeft = () =>
      reads < MAX_READS &&
      Date.now() < deadline &&
      this.state === EmulationState.Paused;
    const coveringRegion = (addr: bigint) =>
      recorded.find((r) => addr >= r.start && addr < r.end);

    // Maps the contiguous readable run starting at the (granule-aligned,
    // readable) `start`, records it, and returns the address just past it.
    const mapFrom = async (start: bigint): Promise<bigint> => {
      let lastOk = start;
      let step = GRAN;
      let hi = start + step;
      while (hi < MAX && budgetLeft() && (await readable(hi))) {
        lastOk = hi;
        step = step < END_STEP_CAP ? step * BigInt(2) : step;
        hi = start + step;
      }
      let bad = hi < MAX ? hi : MAX;
      while (bad - lastOk > GRAN && budgetLeft()) {
        const mid = lastOk + ((bad - lastOk) / GRAN / BigInt(2)) * GRAN;
        if (mid <= lastOk) {
          break;
        }
        if (await readable(mid)) {
          lastOk = mid;
        } else {
          bad = mid;
        }
      }
      const end = lastOk + GRAN;
      regions.push({
        base: "0x" + start.toString(16),
        size: Number(end - start),
        protection: "",
        state: "commit",
        kind: "probed",
        module: null,
      });
      recorded.push({ start, end });
      return end;
    };

    for (const seed of SEEDS) {
      if (!budgetLeft()) {
        break;
      }
      const addr = BigInt(seed) & ALIGN_MASK;
      if (addr < MIN || addr >= MAX || coveringRegion(addr)) {
        continue;
      }
      if (await readable(addr)) {
        await mapFrom(addr);
      }
    }

    // Exhaustive granule sweep: never skips a granule-aligned region.
    let pos = MIN;
    while (pos < MAX && budgetLeft()) {
      const covered = coveringRegion(pos);
      if (covered) {
        pos = covered.end;
        continue;
      }
      if (await readable(pos)) {
        pos = await mapFrom(pos);
      } else {
        pos += GRAN;
      }
    }

    regions.sort((a, b) =>
      BigInt(a.base) < BigInt(b.base)
        ? -1
        : BigInt(a.base) > BigInt(b.base)
          ? 1
          : 0,
    );
    return regions;
  }

  private _flushPendingMemoryRequests() {
    const reads = this.pendingMemoryReads;
    this.pendingMemoryReads = new Map();
    for (const queue of reads.values()) {
      for (const entry of queue) {
        if (!entry.settled) {
          entry.settle(null);
        }
      }
    }

    const regions = this.pendingRegionRequests;
    this.pendingRegionRequests = [];
    for (const resolver of regions) {
      resolver(null);
    }
  }

  getExecutionTime() {
    const endTime = this.pause_time ? this.pause_time : new Date();
    const totalTime = endTime.getTime() - this.start_time.getTime();
    return totalTime - this.paused_time;
  }

  logError(message: string) {
    this.logHandler([`<span class="terminal-red">${message}</span>`]);
  }

  _onError(ev: ErrorEvent) {
    try {
      this.worker.terminate();
    } catch (e) {}

    this.logError(`Emulator encountered fatal error: ${ev.message}`);
    this._flushPendingMemoryRequests();
    this._setState(EmulationState.Failed);
    this.terminateResolve(-1);
  }

  _onMessage(event: MessageEvent) {
    if (event.data.message == "log") {
      this.logHandler(event.data.data);
    } else if (event.data.message == "event") {
      this._onEvent(decodeEvent(event.data.data));
    } else if (event.data.message == "end") {
      this._flushPendingMemoryRequests();
      this._setState(
        this.exit_status === 0 ? EmulationState.Success : EmulationState.Failed,
      );
      this.terminateResolve(this.exit_status);
    }
  }

  _onEvent(event: fbDebugger.DebugEventT) {
    switch (event.eventType) {
      case fbDebugger.Event.GetStateResponse:
        this._handle_state_response(
          event.event as fbDebugger.GetStateResponseT,
        );
        break;
      case fbDebugger.Event.ApplicationExit:
        this._handle_application_exit(
          event.event as fbDebugger.ApplicationExitT,
        );
        break;
      case fbDebugger.Event.EmulationStatus:
        this._handle_emulation_status(
          event.event as fbDebugger.EmulationStatusT,
        );
        break;
      case fbDebugger.Event.ReadMemoryResponse:
        this._handle_read_memory_response(
          event.event as fbDebugger.ReadMemoryResponseT,
        );
        break;
      case fbDebugger.Event.GetMemoryRegionsResponse:
        this._handle_memory_regions_response(
          event.event as fbDebugger.GetMemoryRegionsResponseT,
        );
        break;
    }
  }

  _handle_read_memory_response(response: fbDebugger.ReadMemoryResponseT) {
    const key = response.address.toString(16);
    const queue = this.pendingMemoryReads.get(key);
    if (!queue || queue.length === 0) {
      return;
    }

    // Exactly one response per request, delivered in request order, so the
    // head entry is this response's originator.
    const entry = queue.shift()!;
    if (queue.length === 0) {
      this.pendingMemoryReads.delete(key);
    }

    // Already timed out: this is that request's orphaned late response. Its
    // tombstone consumed it; do not mis-deliver to a live read.
    if (entry.settled) {
      return;
    }

    // The backend only fills `data` when the whole range was readable.
    const data = response.data;
    entry.settle(data.length > 0 ? Uint8Array.from(data) : null);
  }

  _handle_memory_regions_response(
    response: fbDebugger.GetMemoryRegionsResponseT,
  ) {
    const resolver = this.pendingRegionRequests.shift();
    if (!resolver) {
      return;
    }

    let regions: MemoryRegion[] | null = null;
    try {
      const text = new TextDecoder().decode(Uint8Array.from(response.regions));
      regions = JSON.parse(text) as MemoryRegion[];
    } catch (e) {
      console.log(e);
      regions = null;
    }

    resolver(regions);
  }

  _setState(state: EmulationState) {
    this.state = state;

    if (isFinalState(this.state) || this.state === EmulationState.Paused) {
      this.pause_time = new Date();
    } else if (this.state == EmulationState.Running && this.pause_time) {
      this.paused_time += new Date().getTime() - this.pause_time.getTime();
      this.pause_time = null;
    }

    this.stateChangeHandler(this.state);
  }

  _handle_application_exit(info: fbDebugger.ApplicationExitT) {
    this.exit_status = info.exitStatus;
  }

  _handle_emulation_status(info: fbDebugger.EmulationStatusT) {
    this.stautsUpdateHandler({
      activeThreads: info.activeThreads,
      executedInstructions: info.executedInstructions,
      reservedMemory: info.reservedMemory,
      committedMemory: info.committedMemory,
    });
  }

  _handle_state_response(response: fbDebugger.GetStateResponseT) {
    switch (response.state) {
      case fbDebugger.State.None:
        this._setState(EmulationState.Stopped);
        break;

      case fbDebugger.State.Paused:
        this._setState(EmulationState.Paused);
        break;

      case fbDebugger.State.Running:
        this._setState(EmulationState.Running);
        break;
    }
  }
}
