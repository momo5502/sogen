import { createDefaultSettings, Settings, translateSettings } from "./settings";

import * as flatbuffers from "flatbuffers";
import * as fbDebugger from "@/fb/debugger";

type LogHandler = (lines: string[]) => void;

export enum EmulationState {
  Stopped,
  Paused,
  Running,
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

export class Emulator {
  logHandler: LogHandler;
  stateChangeHandler: StateChangeHandler;
  terminatePromise: Promise<number | null>;
  terminateResolve: (value: number | null) => void;
  terminateReject: (reason?: any) => void;
  worker: Worker;
  state: EmulationState = EmulationState.Stopped;

  constructor(logHandler: LogHandler, stateChangeHandler: StateChangeHandler) {
    this.logHandler = logHandler;
    this.stateChangeHandler = stateChangeHandler;
    this.terminateResolve = () => {};
    this.terminateReject = () => {};
    this.terminatePromise = new Promise((resolve, reject) => {
      this.terminateResolve = resolve;
      this.terminateReject = reject;
    });

    this.worker = new Worker(
      /*new URL('./emulator-worker.js', import.meta.url)*/ "./emulator-worker.js",
    );

    this.worker.onmessage = this._onMessage.bind(this);
  }

  async start(settings: Settings = createDefaultSettings(), file: string) {
    this._setState(EmulationState.Running);
    this.worker.postMessage({
      message: "run",
      data: {
        file,
        options: translateSettings(settings),
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

  _onMessage(event: MessageEvent) {
    if (event.data.message == "log") {
      this.logHandler(event.data.data);
    } else if (event.data.message == "event") {
      this._onEvent(decodeEvent(event.data.data));
    } else if (event.data.message == "end") {
      this._setState(EmulationState.Stopped);
      this.terminateResolve(0);
    }
  }

  _onEvent(event: fbDebugger.DebugEventT) {
    switch (event.eventType) {
      case fbDebugger.Event.GetStateResponse:
        this._handle_state_response(
          event.event as fbDebugger.GetStateResponseT,
        );
        break;
    }
  }

  _setState(state: EmulationState) {
    this.state = state;
    this.stateChangeHandler(this.state);
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
