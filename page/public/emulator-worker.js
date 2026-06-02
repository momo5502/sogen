var logLines = [];
var lastFlush = new Date().getTime();

var msgQueue = [];
var pendingUiEvents = [];
const runtimeRoot = "/root-windows";

function ensureDirectory(path) {
  if (!FS.analyzePath(path).exists) {
    FS.mkdirTree(path, 0o777);
  }
}

function getUiEventBridge() {
  if (typeof globalThis.sogen_web_ui_push_event === "function") {
    return globalThis.sogen_web_ui_push_event;
  }

  if (typeof globalThis._sogen_web_ui_push_event === "function") {
    return globalThis._sogen_web_ui_push_event;
  }

  if (typeof globalThis.Module?._sogen_web_ui_push_event === "function") {
    return globalThis.Module._sogen_web_ui_push_event.bind(globalThis.Module);
  }

  return null;
}

function dispatchUiEvent(data) {
  const bridge = getUiEventBridge();
  if (!bridge) {
    logLine(
      `[ui-worker] queue event hwnd=0x${(data.window >>> 0).toString(16)} msg=0x${(data.message >>> 0).toString(16)} w=0x${(data.wParam >>> 0).toString(16)} l=0x${(data.lParam >>> 0).toString(16)}`,
    );
    pendingUiEvents.push(data);
    return false;
  }

  logLine(
    `[ui-worker] dispatch event hwnd=0x${(data.window >>> 0).toString(16)} msg=0x${(data.message >>> 0).toString(16)} w=0x${(data.wParam >>> 0).toString(16)} l=0x${(data.lParam >>> 0).toString(16)}`,
  );
  bridge(
    data.window >>> 0,
    data.message >>> 0,
    data.wParam >>> 0,
    data.lParam >>> 0,
  );
  return true;
}

function flushUiEvents() {
  if (pendingUiEvents.length === 0) {
    return;
  }

  logLine(`[ui-worker] flushing ${pendingUiEvents.length} queued ui event(s)`);
  const events = pendingUiEvents;
  pendingUiEvents = [];
  for (const event of events) {
    if (!dispatchUiEvent(event)) {
      break;
    }
  }
}

onmessage = async (event) => {
  const data = event.data;

  if (data?.type === "sogen_ui_event") {
    dispatchUiEvent(data);
    return;
  }

  const payload = data.data;

  switch (data.message) {
    case "run":
      runEmulation(
        payload.file,
        payload.options,
        payload.arguments,
        payload.persist,
        payload.wasm64,
        payload.cacheBuster,
      );
      break;
    case "event":
      msgQueue.push(payload);
      break;
  }
};

function sendMessage(message, data) {
  postMessage({ message, data });
}

function flushLines() {
  const lines = logLines;
  logLines = [];
  lastFlush = new Date().getTime();
  sendMessage("log", lines);
}

function logLine(text) {
  logLines.push(text);

  const now = new Date().getTime();

  if (lastFlush + 15 < now) {
    flushLines();
  }
}

function notifyExit(code, persist) {
  flushLines();

  const finishExecution = () => {
    sendMessage("end", code);
    self.close();
  };

  if (persist) {
    FS.syncfs(false, finishExecution);
  } else {
    finishExecution();
  }
}

function handleMessage(message) {
  sendMessage("event", message);
}

function getMessageFromQueue() {
  if (msgQueue.length == 0) {
    return "";
  }

  return msgQueue.shift();
}

function runEmulation(
  file,
  options,
  appArguments,
  persist,
  wasm64,
  cacheBuster,
) {
  const mainArguments = [
    ...options,
    "-e",
    "." + runtimeRoot,
    file,
    ...appArguments,
  ];

  globalThis.Module = {
    arguments: mainArguments,
    noInitialRun: true,
    locateFile: (path, scriptDirectory) => {
      const bitness = wasm64 ? "64" : "32";
      const busterParams = cacheBuster ? `?${cacheBuster}` : "";
      return `${scriptDirectory}${bitness}/${path}${busterParams}`;
    },
    onRuntimeInitialized: function () {
      flushUiEvents();
      ensureDirectory(runtimeRoot);
      FS.mount(IDBFS, {}, runtimeRoot);
      FS.syncfs(true, function (_) {
        setTimeout(() => {
          flushUiEvents();
          Module.callMain(mainArguments);
        }, 0);
      });
    },
    print: logLine,
    printErr: logLine,
    onAbort: () => notifyExit(null, persist),
    onExit: (code) => notifyExit(code, persist),
    postRun: flushLines,
  };

  const busterParams = cacheBuster ? `?${cacheBuster}` : "";

  if (wasm64) {
    importScripts("./64/analyzer.js" + busterParams);
  } else {
    importScripts("./32/analyzer.js" + busterParams);
  }
}
