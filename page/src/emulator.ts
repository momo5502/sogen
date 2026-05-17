import { Settings, EmulatorMode, translateSettings } from "./settings";

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
  mode: EmulatorMode;
  state: EmulationState = EmulationState.Stopped;
  exit_status: number | null = null;
  start_time: Date = new Date();
  pause_time: Date | null = null;
  paused_time: number = 0;

  constructor(
    logHandler: LogHandler,
    stateChangeHandler: StateChangeHandler,
    stautsUpdateHandler: StatusUpdateHandler,
    mode: EmulatorMode = "windows",
  ) {
    this.logHandler = logHandler;
    this.stateChangeHandler = stateChangeHandler;
    this.stautsUpdateHandler = stautsUpdateHandler;
    this.mode = mode;
    this.terminateResolve = () => {};
    this.terminateReject = () => {};
    this.terminatePromise = new Promise((resolve, reject) => {
      this.terminateResolve = resolve;
      this.terminateReject = reject;
    });

    const busterParams = cacheBuster ? `?${cacheBuster}` : "";

    const workerScript = mode === "linux"
      ? "./linux-emulator-worker.js"
      : "./emulator-worker.js";

    this.worker = new Worker(workerScript + busterParams);
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
    this._setState(EmulationState.Failed);
    this.terminateResolve(-1);
  }

  _onMessage(event: MessageEvent) {
    if (event.data.message == "log") {
      this.logHandler(event.data.data);
    } else if (event.data.message == "event") {
      this._onEvent(decodeEvent(event.data.data));
    } else if (event.data.message == "end") {
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
    }
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
