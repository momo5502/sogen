"use strict";

// The emulator runs here rather than on the main thread for one reason: FileReaderSync exists only in a
// worker, and it is the only way to read a slice of a multi-gigabyte File synchronously. The range
// bridge the wasm module calls cannot await anything -- it is invoked from inside a memory fault, with
// the guest's execution suspended mid-instruction.

let createSogenMacos = null;

const GUEST_ROOT = "/root";

// Guest path -> File. Holds handles, never contents: a macOS root is several gigabytes and the whole
// point is that only the pages the guest touches are ever read.
const files = new Map();

// Guest path -> URL, for the files served by the local server rather than picked. The bridge has to know
// about these as well: attaching them to the emulated filesystem is not enough, because the emulator
// reads a shared cache through the range bridge and not through the filesystem, and a bridge that knows
// only about picked Files answers "no such file" for every one of them. That reads all the way out as
// dyld reporting no shared cache.
const hostedUrls = new Map();
const hostedSizes = new Map();

// Both are read straight out of the worker's global scope by the wasm module (see the EM_JS bridge in
// main.cpp) rather than passed through a call, because the module is inside a guest instruction at the
// moment it looks: there is no message it could receive and nothing it could await.
//
// The credit is what stops a slow page from being handed frames it will never draw. The module refuses
// to even compose one at zero, so a page that stops acking costs nothing rather than filling the
// structured-clone queue with megabyte buffers.
const FRAME_CREDIT = 2;

// A page can generate mouse-move events far faster than the guest reaches a syscall, and every one of
// them held here is one the guest will eventually see out of date. Beyond this the oldest go.
const INPUT_QUEUE_LIMIT = 256;

globalThis.__sogenFrameCredit = FRAME_CREDIT;
globalThis.__sogenInputQueue = [];
globalThis.__sogenFinished = false;

let inputDropped = 0;

let reader = null;

function ensureReader() {
  if (!reader) reader = new FileReaderSync();
  return reader;
}

// Synchronous by necessity: the bridge is called from inside a guest memory fault, with execution
// suspended mid-instruction, so there is nothing to await into. A synchronous XHR is the only way to
// read a byte range of a served file from a worker, and responseType cannot be set on one -- the
// x-user-defined override is what keeps the response bytes intact through a JS string.
function hostedSize(path) {
  if (hostedSizes.has(path)) return hostedSizes.get(path);

  const url = hostedUrls.get(path);
  if (!url) return -1;

  const request = new XMLHttpRequest();
  request.open("HEAD", url, false);
  request.send(null);

  const size =
    request.status >= 200 && request.status < 300
      ? Number(request.getResponseHeader("Content-Length") || 0)
      : -1;
  hostedSizes.set(path, size);
  return size;
}

function fetchRange(url, start, length) {
  if (length <= 0) return new Uint8Array(0);

  const request = new XMLHttpRequest();
  request.open("GET", url, false);
  request.setRequestHeader("Range", `bytes=${start}-${start + length - 1}`);

  // A response type on a synchronous request is refused only in a window context, and this is a worker,
  // so the range can arrive as bytes. It is worth the fallback: taking a 2 MiB page-in as
  // x-user-defined text builds a two-million-character UTF-16 string and copies it back one charCodeAt
  // at a time, which measured as a fifth of a whole run.
  let binary = false;
  try {
    request.responseType = "arraybuffer";
    binary = request.responseType === "arraybuffer";
  } catch {
    /* a context that refuses it falls through to the text path below */
  }

  if (!binary) request.overrideMimeType("text/plain; charset=x-user-defined");

  request.send(null);

  if (request.status !== 206 && request.status !== 200) {
    throw new Error(
      `HTTP ${request.status} for bytes ${start}-${start + length - 1}`,
    );
  }

  if (binary) return new Uint8Array(request.response);

  const text = request.responseText;
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; ++i) bytes[i] = text.charCodeAt(i) & 0xff;
  return bytes;
}

// The bridge is called from inside a guest memory fault, so every read is one synchronous HTTP request
// with nothing to batch it against, and its two callers ask for opposite things: the chunk pager takes
// the shared cache 2 MiB at a time, while the cache header is walked a couple of bytes at a time.
// Measured on the paint demo: 14,208 reads, 13,600 of them under a kilobyte, together 11.5 s of a 15 s
// run, and 7,308 of them landing in one 573 KB file. Anything smaller than a block is therefore answered
// out of whole blocks, so only a first touch costs a request; anything already block-sized is passed
// straight through, which keeps a page-in at one request and stops bulk paging from evicting the blocks
// a header parser is walking.
// The same grid the service worker keys its cache on, and the same size the guest's pager reads in, so
// a small-read block and a page-in and a cached block are all one aligned 2 MiB extent. Mismatched
// grids cost real bytes: at 1 MiB here against 2 MiB there, every small read pulled a 2 MiB block to
// answer a 1 MiB request, and a cold run fetched 1058 MiB instead of 692 MiB.
const RANGE_BLOCK_SIZE = 2 << 20;
const RANGE_BLOCK_BUDGET = 64 << 20;

const rangeBlocks = new Map();
let rangeBlockBytes = 0;

function rangeBlock(url, path, index) {
  const key = `${path}#${index}`;
  const cached = rangeBlocks.get(key);
  if (cached) return cached;

  const total = hostedSize(path);
  const base = index * RANGE_BLOCK_SIZE;
  const block =
    total < 0 || base >= total
      ? new Uint8Array(0)
      : fetchRange(url, base, Math.min(RANGE_BLOCK_SIZE, total - base));

  // Insertion order is eviction order. A parser walks a header forwards, so the block held longest is
  // the one least likely to be asked for again.
  while (
    rangeBlockBytes + block.length > RANGE_BLOCK_BUDGET &&
    rangeBlocks.size > 0
  ) {
    const oldest = rangeBlocks.keys().next().value;
    rangeBlockBytes -= rangeBlocks.get(oldest).length;
    rangeBlocks.delete(oldest);
  }

  rangeBlocks.set(key, block);
  rangeBlockBytes += block.length;
  return block;
}

function hostedRange(path, start, length) {
  const url = hostedUrls.get(path);
  if (!url) return null;

  if (length >= RANGE_BLOCK_SIZE) return fetchRange(url, start, length);

  const first = Math.floor(start / RANGE_BLOCK_SIZE);
  const last = Math.floor((start + length - 1) / RANGE_BLOCK_SIZE);

  if (first === last) {
    const block = rangeBlock(url, path, first);
    const offset = start - first * RANGE_BLOCK_SIZE;
    return block.subarray(
      Math.min(offset, block.length),
      Math.min(offset + length, block.length),
    );
  }

  const out = new Uint8Array(length);
  let written = 0;

  for (let index = first; index <= last; ++index) {
    const block = rangeBlock(url, path, index);
    const base = index * RANGE_BLOCK_SIZE;
    const from = Math.max(start, base) - base;
    const to = Math.min(start + length, base + block.length) - base;
    if (to <= from) break;

    out.set(block.subarray(from, to), written);
    written += to - from;
  }

  return out.subarray(0, written);
}

function installBridge() {
  // A run may attach a different URL under a path the last one used, and a block is keyed by path.
  rangeBlocks.clear();
  rangeBlockBytes = 0;

  globalThis.sogenHostRangeSize = (path) => {
    const file = files.get(path);
    if (file) return file.size;
    return hostedSize(path);
  };

  globalThis.sogenHostRangeRead = (path, offset, size) => {
    const start = Number(offset);
    const length = Number(size);

    const file = files.get(path);
    const total = file ? file.size : hostedSize(path);
    if (total < 0) return -1;
    if (start >= total || length <= 0) return new Uint8Array(0);

    const end = Math.min(start + length, total);

    try {
      if (file) {
        return new Uint8Array(
          ensureReader().readAsArrayBuffer(file.slice(start, end)),
        );
      }

      return hostedRange(path, start, end - start) || new Uint8Array(0);
    } catch (error) {
      post({
        t: "bridge-error",
        path,
        offset: start,
        size: length,
        message: String(error),
      });
      return new Uint8Array(0);
    }
  };
}

// page/src/emulator.ts::_onMessage only recognises postMessage({ message, data }); everything this
// worker sends is funnelled through here to speak that shape. The wasm module itself does not go
// through this function -- sogen_web_frame and sogen_web_finished (see main.cpp) call the bare
// postMessage identifier directly, because they are EM_JS bodies with no access to this file's
// scope -- so the global is overridden below rather than only wrapped, which is what lets a run's own
// completion notice reach the page instead of arriving as an event type _onMessage has never heard of.
const nativePostMessage = self.postMessage.bind(self);

// page/src/emulator.ts only sets exit_status by decoding this "event" message as a flatbuffers
// Debugger.DebugEvent (src/debugger/events.fbs, page/src/fb/debugger/*.ts) carrying an ApplicationExit
// payload -- exit_status is what decides Success vs Failed and what terminateResolve() hands back, and
// nothing else sets it. Windows and linux produce this for free: their wasm module links src/debugger
// and calls send_event(ApplicationExitT{...}) itself (event_handler.cpp:877-879), over the same
// EM_ASM(handleMessage(...)) bridge sogen_web_finished uses here for "done". macos-web links no such
// library, so the bytes are built here instead of left unsent.
//
// DebugEvent{event_type: ApplicationExit (13), event: ApplicationExit{exit_status: uint32}} is a table
// wrapping a single-scalar-field table, so its vtable and layout never depend on the field's value --
// only the last 4 bytes (exit_status itself, little-endian) ever change. Verified against
// flatbuffers.Builder across a spread of exit codes (0, 1, -1, 255, 4294967295): identical 36-byte
// buffers apart from that one word.
const APPLICATION_EXIT_EVENT_PREFIX = new Uint8Array([
  0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x09, 0x00, 0x04, 0x00, 0x08,
  0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x06, 0x00, 0x08, 0x00,
  0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
]);

function encodeApplicationExitEvent(exitStatus) {
  const bytes = new Uint8Array(36);
  bytes.set(APPLICATION_EXIT_EVENT_PREFIX);
  new DataView(bytes.buffer).setInt32(32, exitStatus | 0, true);

  let binary = "";
  for (let i = 0; i < bytes.length; ++i)
    binary += String.fromCharCode(bytes[i]);
  return btoa(binary);
}

// transfer is what lets sogen_web_frame's pixel buffer cross into the page as a move rather than a
// structured-clone copy of a multi-megabyte ArrayBuffer; the "frame" case below is the one payload that
// ever carries one.
function post(payload, transfer) {
  switch (payload.t) {
    // Two page-bound messages from one worker-bound one, and the only case that produces two. The "log"
    // half is the terminal's: page/src/emulator.ts's Output panel is what windows and linux run against
    // and it takes plain text. The event half is page/src/macos/trace-view.tsx's, which renders the
    // module's own vocabulary and would otherwise have no account of the lines that are not events --
    // this worker's own attach diagnostics, and any emscripten warning or assertion.
    case "line":
      nativePostMessage({ message: "log", data: [payload.text] });
      nativePostMessage({
        message: "macos-status",
        event: {
          t: "line",
          text: payload.text,
          error: payload.isError ? 1 : 0,
        },
      });
      break;

    // Not also sent as a log line. It used to round-trip through JSON because the Output panel had no
    // renderer for this vocabulary; trace-view.tsx is that renderer, and page/src/playground.tsx mounts
    // it instead of the Output panel in macOS mode, so a second copy as text would be paid for and
    // read by nobody.
    case "event":
      nativePostMessage({ message: "macos-status", event: payload.event });
      break;

    case "bridge-error":
      post({
        t: "line",
        isError: true,
        text: `range read failed at ${payload.path} +${payload.offset} (${payload.size} bytes): ${payload.message}`,
      });
      break;

    case "done":
      nativePostMessage({
        message: "event",
        data: encodeApplicationExitEvent(payload.code),
      });
      nativePostMessage({ message: "end", data: payload.code });
      break;

    // Forwarded raw rather than wrapped in page's {message, data} envelope: guest-screen.tsx listens for
    // these two shapes directly (window.addEventListener("message", ...) on the same worker
    // page/src/emulator.ts already owns), and "frame" in particular has to keep its transfer list to stay
    // zero-copy.
    case "frame":
      nativePostMessage(payload, transfer);
      break;

    case "input-dropped":
      nativePostMessage(payload);
      break;

    default:
      break;
  }
}

self.postMessage = (payload, transfer) => post(payload, transfer);

// The server marks a directory with a trailing slash on the href, a symlinked one included, so nothing
// here resolves a link: an emulation root is built out of them and following one leads back out of it.
function servedListing(url) {
  const request = new XMLHttpRequest();
  request.open("GET", `${url}/`, false);
  request.send(null);

  if (request.status < 200 || request.status >= 300) return null;

  return [...request.responseText.matchAll(/href="([^"?]+)"/g)].map((match) => {
    const directory = match[1].endsWith("/");
    const href = directory ? match[1].slice(0, -1) : match[1];
    return { href, name: decodeURIComponent(href), directory };
  });
}

let announcedServedRoot = false;

// Whatever the page did not attach by hand is resolved against the served root the first time the guest
// misses on it: one directory listing per directory it enters, and a lazy file per entry in that
// listing. A macOS root holds millions of files and an app reads a few hundred, so nothing can be
// enumerated up front -- and the few hundred an app bundle needs are exactly the ones the shared cache
// does not carry. AppKit's +[NSAppearance _initializeCoreUI] reads .car catalogues out of
// /System/Library/CoreServices and throws before its first window if they are not there.
function installServedRoot(instance, root, base, directories) {
  const { FS } = instance;

  const populate = (node) => {
    if (node.servedPopulated) return false;
    node.servedPopulated = true;

    const listing = servedListing(node.servedUrl);
    if (!listing) return false;

    if (!announcedServedRoot) {
      announcedServedRoot = true;
      post({
        t: "line",
        text: `the served root at ${base} is answering what the page did not attach`,
        isError: false,
      });
    }

    for (const entry of listing) {
      const url = `${node.servedUrl}/${entry.href}`;
      const existing = Object.hasOwn(node.contents, entry.name)
        ? node.contents[entry.name]
        : null;

      if (existing) {
        if (FS.isDir(existing.mode)) serve(existing, url);
        continue;
      }

      const path = `${FS.getPath(node)}/${entry.name}`;

      try {
        if (entry.directory) {
          FS.mkdir(path);
          serve(FS.lookupPath(path).node, url);
        } else {
          FS.createLazyFile(node, entry.name, url, true, false);

          // The range bridge reads a mapped file straight off the host rather than through the
          // filesystem, and it knows nothing about a node the filesystem materialised on its own.
          hostedUrls.set(path, url);
        }
      } catch (error) {
        post({
          t: "line",
          text: `could not attach ${path} from the served root: ${error}`,
          isError: true,
        });
      }
    }

    return true;
  };

  const serve = (node, url) => {
    if (node.servedUrl) return;
    node.servedUrl = url;

    const ops = node.node_ops;
    node.node_ops = Object.create(ops);

    // Only ever reached on a miss: the filesystem keeps its own name table and calls into a directory's
    // lookup solely when nothing in it matched. So a name arriving here is one that is not in memory
    // yet, never one that has already been ruled out.
    node.node_ops.lookup = (parent, name) =>
      populate(parent) ? FS.lookupNode(parent, name) : ops.lookup(parent, name);
    node.node_ops.readdir = (dir) => {
      populate(dir);
      return ops.readdir(dir);
    };

    // The page's own attachments already built a few directories under the root, and a directory that
    // exists in memory would otherwise never ask the server about the names it is missing.
    for (const [name, child] of Object.entries(node.contents)) {
      if (FS.isDir(child.mode))
        serve(child, `${url}/${encodeURIComponent(name)}`);
    }
  };

  // Before the root, so the root's own descent finds these already served and leaves them alone: a
  // subtree the page mounted somewhere other than where it sits on the server would otherwise be handed
  // the URL its guest path spells, which resolves to nothing.
  for (const entry of directories || []) {
    try {
      serve(FS.lookupPath(entry.path).node, entry.url);
    } catch (error) {
      post({
        t: "line",
        text: `could not point ${entry.path} at ${entry.url}: ${error}`,
        isError: true,
      });
    }
  }

  serve(FS.lookupPath(root).node, base);
}

// The module writes one JSON object per line; anything else is passed through so an emscripten warning
// or an assertion is visible rather than swallowed.
let buffered = "";

function emit(text, isError) {
  buffered += text + "\n";
  let newline;
  while ((newline = buffered.indexOf("\n")) >= 0) {
    const line = buffered.slice(0, newline).trim();
    buffered = buffered.slice(newline + 1);
    if (!line) continue;

    if (line.startsWith("{") && line.endsWith("}")) {
      try {
        post({ t: "event", event: JSON.parse(line) });
        continue;
      } catch {
        /* fall through */
      }
    }
    post({ t: "line", text: line, isError: Boolean(isError) });
  }
}

async function run({
  args,
  memfs,
  root,
  hosted,
  hostedDirs,
  served,
  embeddedDemo,
  cacheBuster,
}) {
  const busterParams = cacheBuster ? `?${cacheBuster}` : "";

  if (!createSogenMacos) {
    ({ default: createSogenMacos } = await import(
      `./macos-analyzer.js${busterParams}`
    ));
  }

  installBridge();

  const instance = await createSogenMacos({
    print: (text) => emit(text, false),
    printErr: (text) => emit(text, true),

    // The glue's one locateFile call asks for "macos-web.wasm", the macos-web CMake target's own
    // OUTPUT_NAME baked in at link time, which is not the name page/public serves the binary under.
    locateFile: (_path, scriptDirectory) =>
      `${scriptDirectory}macos-analyzer.wasm${busterParams}`,
  });

  // The three demos are embedded at a fixed absolute path (see the --embed-file entries in
  // src/macos-web/CMakeLists.txt) that every fresh instance already carries, unrelated to whatever
  // --root this run uses. load_executable resolves --exe relative to --root the same way
  // describe_image's own preview does, so the bytes are copied under root before main() ever looks for
  // them -- exactly what the standalone page's "Add demo binary" does by hand, minus the throwaway
  // instance that page needs only because it inspects the bytes before a worker exists at all.
  if (embeddedDemo) {
    try {
      const data = instance.FS.readFile(embeddedDemo.source);
      const slash = embeddedDemo.guestPath.lastIndexOf("/");
      if (slash > 0)
        instance.FS.mkdirTree(embeddedDemo.guestPath.slice(0, slash));
      instance.FS.writeFile(embeddedDemo.guestPath, data);
    } catch (error) {
      post({
        t: "line",
        text: `could not stage the embedded demo ${embeddedDemo.source}: ${error}`,
        isError: true,
      });
    }
  }

  for (const [path, data] of memfs) {
    const full = root + path;
    const slash = full.lastIndexOf("/");
    if (slash > 0) instance.FS.mkdirTree(full.slice(0, slash));
    instance.FS.writeFile(full, data);
  }

  // Files served by the local server are attached lazily rather than fetched: createLazyFile reads the
  // ranges the guest actually touches over HTTP, which is the same bargain WORKERFS makes with a local
  // File. It is what lets a 5.4 GB shared cache be "loaded" instantly.
  for (const entry of hosted || []) {
    const slash = entry.path.lastIndexOf("/");
    const directory = slash > 0 ? entry.path.slice(0, slash) : "/";
    const name = entry.path.slice(slash + 1);

    try {
      instance.FS.mkdirTree(directory);
      instance.FS.createLazyFile(directory, name, entry.url, true, false);
      hostedUrls.set(entry.path, entry.url);

      // Attaching a lazy file only records where to fetch from; nothing is read and nothing can fail
      // until the guest touches it, and by then the failure surfaces as dyld reporting no shared cache
      // rather than as a file that could not be read. One byte through the real path settles it here.
      const stat = instance.FS.stat(entry.path);
      const handle = instance.FS.open(entry.path, "r");
      const probe = new Uint8Array(4);
      const got = instance.FS.read(handle, probe, 0, probe.length, 0);
      instance.FS.close(handle);

      if (!stat.size || got !== probe.length) {
        post({
          t: "line",
          isError: true,
          text: `attached ${entry.path} but it reads as ${stat.size} bytes / ${got} readable -- the guest will see no such file`,
        });
      }
    } catch (error) {
      post({
        t: "line",
        text: `could not attach ${entry.path}: ${error}`,
        isError: true,
      });
    }
  }

  // Picked files are written into the module's own filesystem rather than mounted. A WORKERFS mount
  // replaces the directory it is mounted on, so mounting one at /root would wipe the lazily attached
  // dylinker and cache that live under it -- which looked exactly like dyld being unreadable.
  //
  // That makes size the constraint here, so it is checked rather than discovered: a root belongs on the
  // server, where it costs nothing.
  const COPY_LIMIT = 256 * 1024 * 1024;

  for (const [path, file] of files) {
    if (file.size > COPY_LIMIT) {
      post({
        t: "line",
        text: `${path} is ${(file.size / 1048576).toFixed(0)} MiB, too large to add this way; serve it with --macos-root instead`,
        isError: true,
      });
      continue;
    }

    try {
      const slash = path.lastIndexOf("/");
      if (slash > 0) instance.FS.mkdirTree(path.slice(0, slash));
      instance.FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
    } catch (error) {
      post({
        t: "line",
        text: `could not add ${path}: ${error}`,
        isError: true,
      });
    }
  }

  if (served) installServedRoot(instance, root, served, hostedDirs);

  // Deliberately not the source of the terminal message, and its return value is deliberately unused:
  // the first time the frame pump yields, ASYNCIFY unwinds the stack and callMain returns 0 with the run
  // still going. Reading that as "finished" is exactly what tore a live run down. main() reports its own
  // exit through the bridge instead, and a run that never gets there throws out of here.
  instance.callMain(args);
  emit("", false);
}

// A served macOS root (page/src/macos/root-cache.ts's discoverRootFiles()) reaches this worker through
// its own pre-run message rather than through the "run" payload itself: page/src/emulator.ts owns that
// payload's shape and does not know about a macOS root, so guest-screen.tsx posts it directly to this
// worker -- see "attach-root" below -- and it is held here until the next run picks it up.
let attachedHosted = [];
let attachedServed = null;

// Guest directory -> the directory on the served root it stands for, for the subtrees where the two are
// not the same path. page/src/macos/bundle-attach.tsx mounts a bundle where a launched app would appear
// -- /Applications/<name>.app -- while on the server it stays wherever the system keeps it, which for a
// shipped app is under /System. Without this installServedRoot resolves a name the page did not attach
// against /Applications/<name>.app on the server, where there is nothing.
let attachedDirs = [];

// page's own run message carries a windows/linux-shaped payload -- an emulator-switch list translated
// by settings.ts's translateSettings, plus the guest program's own argv. settings.ts's macOS branch
// produces macos-web's own flags (--no-decode, --gui, --desktop-size=, ...), not the windows ones, so
// they are forwarded as given rather than reinterpreted here. The guest argv translates directly: this
// is exactly what buildArguments() in the standalone page's app.js does with --arg for every element of
// a launched program's own command line.
// The three probes page/src/macos/guest-screen.tsx offers are embedded in the module (see
// --embed-file in src/macos-web/CMakeLists.txt) at these fixed absolute paths, not under any --root.
// Selecting one by that path is more convenient for the page than staging it under root by hand, so it
// is recognised here and copied into place in run() instead.
const EMBEDDED_DEMOS = new Set([
  "/demo/macho_trace_arm64",
  "/demo/macho_calcdemo_arm64",
  "/demo/macho_paintprobe_arm64",
]);

function runOptionsFor({
  file,
  options,
  arguments: appArguments,
  cacheBuster,
}) {
  const embedded = EMBEDDED_DEMOS.has(file);
  const exe = embedded ? `/bin/${file.split("/").pop()}` : file;

  return {
    args: [
      `--exe=${exe}`,
      `--root=${GUEST_ROOT}`,
      ...(options || []),
      `--arg=${exe}`,
      ...appArguments.map((value) => `--arg=${value}`),
    ],
    memfs: [],
    root: GUEST_ROOT,
    hosted: attachedHosted,
    hostedDirs: attachedDirs,
    served: attachedServed,
    embeddedDemo: embedded
      ? {
          source: file,
          guestPath: `${GUEST_ROOT}/bin/${file.split("/").pop()}`,
        }
      : null,
    cacheBuster,
  };
}

onmessage = async (message) => {
  const data = message.data;

  // guest-screen.tsx speaks to this worker directly, the same untranslated shape
  // src/macos-web/app.js's sendInput()/frame ack used against the standalone worker, rather than
  // through page's {message, data} envelope: there is nothing for emulator.ts to translate here, and
  // giving it one would be one more shape it has to not choke on.
  if (data.t === "input") {
    const queue = globalThis.__sogenInputQueue;
    queue.push(data.event);
    if (queue.length > INPUT_QUEUE_LIMIT) {
      inputDropped += queue.length - INPUT_QUEUE_LIMIT;
      queue.splice(0, queue.length - INPUT_QUEUE_LIMIT);
      post({ t: "input-dropped", count: inputDropped });
    }
    return;
  }

  if (data.t === "frame-ack") {
    globalThis.__sogenFrameCredit = Math.min(
      FRAME_CREDIT,
      globalThis.__sogenFrameCredit + 1,
    );
    return;
  }

  if (data.t === "attach-root") {
    attachedHosted = data.hosted || [];
    attachedDirs = data.dirs || [];
    attachedServed = data.served || null;
    return;
  }

  // Handles, never contents: a picked File is read a slice at a time by the range bridge above, which is
  // what lets a whole .app bundle -- or a root -- be attached without copying any of it.
  if (data.t === "files") {
    files.clear();
    for (const [path, file] of data.files) files.set(path, file);
    return;
  }

  const payload = data.data;

  switch (data.message) {
    case "run":
      globalThis.__sogenFrameCredit = FRAME_CREDIT;
      globalThis.__sogenInputQueue.length = 0;
      globalThis.__sogenFinished = false;
      inputDropped = 0;

      try {
        await run(runOptionsFor(payload));
      } catch (error) {
        post({
          t: "line",
          text: `worker error: ${error && error.message ? error.message : error}`,
          isError: true,
        });
        post({ t: "done", code: -1 });
      }
      break;

    case "event":
      // page's debugger protocol (pause/resume/memory reads, flatbuffer-encoded) has no counterpart
      // here: the wasm module exports no bridge to poll for one, unlike the windows/linux builds this
      // message type is designed for. Nothing to do until that bridge exists.
      break;
  }
};
