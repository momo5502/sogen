onmessage = async (event) => {
    const data = event.data;
    if (data.message == "run") {
        const payload = data.data;
        runEmulation(payload.filesystem, payload.file);
    }
};

function logLine(text) {
    postMessage({ message: "log", data: text });
}

function runEmulation(filesystem, file) {
    globalThis.Module = {
        arguments: ["-b", /*"-c",*/ "-e", "./root", file],
        onRuntimeInitialized: function () {
            filesystem.forEach(e => {
                if (e.name.endsWith("/")) {
                    FS.mkdir(e.name.slice(0, -1));
                } else {
                    const dirs = e.name.split("/")
                    const file = dirs.pop();
                    const buffer = new Uint8Array(e.data);
                    if (FS.analyzePath(e.name).exists) {
                        FS.unlink(e.name);
                    }
                    FS.createDataFile("/" + dirs.join('/'), file, buffer, true, true);
                }
            })
        },
        print: logLine,
        printErr: logLine,
        postRun: () => {
            postMessage({ message: "end", data: null });
            self.close();
        },
    };

    importScripts('./analyzer.js?1');
}
