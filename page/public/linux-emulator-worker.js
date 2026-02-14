var logLines = [];
var lastFlush = new Date().getTime();

var msgQueue = [];

onmessage = async (event) => {
  const data = event.data;
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
  const mainArguments = [...options, "--root", "./root", file, ...appArguments];

  globalThis.Module = {
    arguments: mainArguments,
    noInitialRun: true,
    locateFile: (path, scriptDirectory) => {
      const bitness = wasm64 ? "linux64" : "linux32";
      const busterParams = cacheBuster ? `?${cacheBuster}` : "";
      return `${scriptDirectory}${bitness}/${path}${busterParams}`;
    },
    onRuntimeInitialized: function () {
      FS.mkdir("/root");
      FS.mount(IDBFS, {}, "/root");
      FS.syncfs(true, function (_) {
        setTimeout(() => {
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
    importScripts("./linux64/linux-analyzer.js" + busterParams);
  } else {
    importScripts("./linux32/linux-analyzer.js" + busterParams);
  }
}
