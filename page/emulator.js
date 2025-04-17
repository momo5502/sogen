onmessage = async (event) => {
    const data = event.data;
    if (data.message == "run") {
        runEmulation(data.data);
    }
};

function logLine(text) {
    postMessage(text);
}

function runEmulation(filesystem) {
    globalThis.Module = {
        arguments: ["-b", "-c", "-e", "./root", "c:/lul.exe",],
        onRuntimeInitialized: function () {
            filesystem.forEach(e => {
                if (e.name.endsWith("/")) {
                    FS.mkdir(e.name.slice(0, -1));
                } else {
                    const dirs = e.name.split("/")
                    const file = dirs.pop();
                    const buffer = new Uint8Array(e.data);
                    FS.createDataFile("/" + dirs.join('/'), file, buffer, true, true);
                }
            })
        },
        print: logLine,
        printErr: logLine,
        postRun: () => {
            self.close();
        },
    };

    importScripts('./analyzer.js?1');
}
