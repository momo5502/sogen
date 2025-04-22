var logLines = [];
var lastFlush = new Date().getTime();

onmessage = async (event) => {
  const data = event.data;
  if (data.message == "run") {
    const payload = data.data;
    runEmulation(payload.filesystem, payload.file, payload.options);
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

function runEmulation(filesystem, file, options) {
  globalThis.Module = {
    arguments: [...options, "-e", "./root", file],
    onRuntimeInitialized: function () {
      filesystem.forEach((e) => {
        if (e.name.endsWith("/")) {
          FS.mkdir(e.name.slice(0, -1));
        } else {
          const dirs = e.name.split("/");
          const file = dirs.pop();
          const buffer = new Uint8Array(e.data);
          if (FS.analyzePath(e.name).exists) {
            FS.unlink(e.name);
          }
          FS.createDataFile("/" + dirs.join("/"), file, buffer, true, true);
        }
      });
    },
    print: logLine,
    printErr: logLine,
    postRun: () => {
      flushLines();
      postMessage({ message: "end", data: null });
      self.close();
    },
  };

  importScripts("./analyzer.js?1");
}
