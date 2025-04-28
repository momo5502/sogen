import { createDefaultSettings, Settings, translateSettings } from "./settings";
import { FileEntry } from "./zip-file";

import * as flatbuffers from "flatbuffers";
import * as fbDebugger from "@/fb/debugger";
import * as fbDebuggerEvent from "@/fb/debugger/event";

type LogHandler = (lines: string[]) => void;

export interface UserFile {
  name: string;
  data: ArrayBuffer;
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

export class Emulator {
  filesystem: FileEntry[];
  logHandler: LogHandler;
  terminatePromise: Promise<number | null>;
  terminateResolve: (value: number | null) => void;
  terminateReject: (reason?: any) => void;
  worker: Worker;

  constructor(filesystem: FileEntry[], logHandler: LogHandler) {
    this.filesystem = filesystem;
    this.logHandler = logHandler;
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

  start(
    settings: Settings = createDefaultSettings(),
    userFile: UserFile | null = null,
  ) {
    var file = "c:/test-sample.exe";
    if (userFile) {
      const filename = userFile.name.split("/").pop()?.split("\\").pop();
      const canonicalName = filename?.toLowerCase();
      file = "c:/" + canonicalName;
      this.filesystem.push({
        name: "root/filesys/c/" + canonicalName,
        data: userFile.data,
      });
    }

    this.worker.postMessage({
      message: "run",
      data: {
        filesystem: this.filesystem,
        file,
        options: translateSettings(settings),
      },
    });
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
  }

  resume() {
    this.sendEvent(
      new fbDebugger.DebugEventT(
        fbDebugger.Event.RunRequest,
        new fbDebugger.RunRequestT(),
      ),
    );
  }

  _onMessage(event: MessageEvent) {
    if (event.data.message == "log") {
      this.logHandler(event.data.data);
    } else if (event.data.message == "event") {
      this._onEvent(decodeEvent(event.data.data));
    } else if (event.data.message == "end") {
      this.terminateResolve(0);
    }
  }

  _onEvent(event: fbDebugger.DebugEventT) {
    console.log(event);
  }
}
