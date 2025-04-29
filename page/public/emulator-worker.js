var logLines = [];
var lastFlush = new Date().getTime();

var msgQueue = [];

onmessage = async (event) => {
  const data = event.data;
  if (data.message == "run") {
    const payload = data.data;
    runEmulation(payload.file, payload.options);
  } else if (data.message == "event") {
    const payload = data.data;
    msgQueue.push(payload);
  }
};

function flushLines() {
  const lines = logLines;
  logLines = [];
  lastFlush = new Date().getTime();
  postMessage({ message: "log", data: lines });
}

function logLine(text) {
  logLines.push(text);

  const now = new Date().getTime();

  if (lastFlush + 15 < now) {
    flushLines();
  }
}

function notifyExit(code) {
  flushLines();
  postMessage({ message: "end", data: code });
  self.close();
}

function handleMessage(message) {
  postMessage({ message: "event", data: message });
}

function getMessageFromQueue() {
  if (msgQueue.length == 0) {
    return "";
  }

  return msgQueue.shift();
}

function runEmulation(file, options) {
  const mainArguments = [...options, "-e", "./root", file];

  globalThis.Module = {
    arguments: mainArguments,
    noInitialRun: true,
    onRuntimeInitialized: function () {
      FS.mkdir("/root");
      FS.mount(IDBFS, {}, "/root");
      FS.syncfs(true, function (err) {
        Module.callMain(mainArguments);
      });
    },
    print: logLine,
    printErr: logLine,
    onAbort: () => notifyExit(null),
    onExit: notifyExit,
    postRun: flushLines,
  };

  importScripts("./analyzer.js");
}
