import { createDefaultSettings, Settings, translateSettings } from "./settings";
import { FileEntry } from "./zip-file";

type LogHandler = (lines: string[]) => void;

export interface UserFile {
  name: string;
  data: ArrayBuffer;
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

    this.worker.onmessage = (event: MessageEvent) => {
      if (event.data.message == "log") {
        this.logHandler(event.data.data);
      } else if (event.data.message == "end") {
        this.terminateResolve(0);
      }
    };
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
}
