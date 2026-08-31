'use strict';

// Runs in a worker on purpose: synchronous XHR and FileReaderSync are the two APIs the shared-cache
// range bridge would use, and neither is available (or legal) on the main thread.

function probeSyncXhrResponseType() {
  try {
    const xhr = new XMLHttpRequest();
    xhr.open('GET', 'data:application/octet-stream,', false);
    xhr.responseType = 'arraybuffer';
    return { ok: true, detail: 'responseType=arraybuffer accepted' };
  } catch (e) {
    return { ok: false, detail: String(e) };
  }
}

function probeSyncXhrRange(origin) {
  try {
    const xhr = new XMLHttpRequest();
    xhr.open('GET', origin + '/capability-probe-worker.js', false);
    try { xhr.responseType = 'arraybuffer'; } catch { /* measured separately above */ }
    xhr.setRequestHeader('Range', 'bytes=0-15');
    xhr.send(null);

    const body = xhr.response;
    const length = body instanceof ArrayBuffer ? body.byteLength : String(xhr.responseText || '').length;
    return {
      ok: xhr.status === 206 && length === 16,
      detail: `status=${xhr.status} bytes=${length} content-range="${xhr.getResponseHeader('Content-Range') || ''}"`,
    };
  } catch (e) {
    return { ok: false, detail: String(e) };
  }
}

// The question is whether a slice taken past the 4 GiB mark can be read at all, so a file at or below
// that size cannot answer it and says so rather than reporting a pass.
function probeFileReaderSync(file) {
  if (typeof FileReaderSync !== 'function') {
    return { ok: false, detail: 'FileReaderSync missing' };
  }
  if (!file) {
    return { ok: false, detail: 'no file picked' };
  }

  const FOUR_GIB = 4 * 1024 * 1024 * 1024;
  const window = 2 * 1024 * 1024;

  try {
    const offset = Math.max(0, file.size - window);
    const started = performance.now();
    const buffer = new FileReaderSync().readAsArrayBuffer(file.slice(offset, offset + window));
    const elapsed = performance.now() - started;

    const past = offset > FOUR_GIB;
    const expected = Math.min(window, file.size);
    return {
      ok: buffer.byteLength === expected,
      detail:
        `size=${file.size} offset=${offset} bytes=${buffer.byteLength} ` +
        `ms=${elapsed.toFixed(2)} (${(window / 1048576 / (elapsed / 1000)).toFixed(0)} MiB/s) ` +
        (past ? 'offset IS past 4 GiB' : 'offset is NOT past 4 GiB - inconclusive, pick a larger file'),
    };
  } catch (e) {
    return { ok: false, detail: String(e) };
  }
}

// Two different ceilings, and only one of them is the emulator's.
//
// `new WebAssembly.Memory({index:"i64", maximum})` is capped far lower by the JS API than a maximum
// declared inside a module is. Measuring the constructor -- which is what the obvious probe does --
// therefore reports a ceiling the emulator never runs into, and would cap the build flag at a fraction
// of what the engine actually allows. The module path is measured by assembling a tiny wasm binary that
// declares nothing but a memory, which is the shape the real module has.
function buildMemoryModule(maxPages) {
  const uleb = (value) => {
    const out = [];
    do {
      let byte = value & 0x7f;
      value >>>= 7;
      if (value) byte |= 0x80;
      out.push(byte);
    } while (value);
    return out;
  };

  const section = (id, payload) => [id, ...uleb(payload.length), ...payload];
  const name = [...'memory'].map((c) => c.charCodeAt(0));

  return Uint8Array.from([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...section(5, [0x01, 0x05, ...uleb(1), ...uleb(maxPages)]), // 0x05 = has-maximum | memory64
    ...section(7, [0x01, name.length, ...name, 0x02, 0x00]),
  ]);
}

function probeMemory64Maximum() {
  const PAGE = 65536;
  const GIB = 1024 * 1024 * 1024;

  const constructorAccepts = (pages) => {
    try {
      new WebAssembly.Memory({ initial: 1, maximum: pages, index: 'i64' });
      return true;
    } catch {
      return false;
    }
  };

  const moduleAccepts = (pages) => {
    try {
      new WebAssembly.Instance(new WebAssembly.Module(buildMemoryModule(pages)));
      return true;
    } catch {
      return false;
    }
  };

  if (!moduleAccepts(1)) {
    return { ok: false, detail: 'memory64 not supported' };
  }

  const bisect = (accepts) => {
    let low = 1;
    let high = 1 << 20; // 64 GiB in pages
    while (low < high) {
      const mid = Math.floor((low + high + 1) / 2);
      if (accepts(mid)) low = mid; else high = mid - 1;
    }
    return low;
  };

  const byModule = bisect(moduleAccepts);
  const byConstructor = constructorAccepts(1) ? bisect(constructorAccepts) : 0;

  // Declaring a maximum is not the same as being able to grow into it, and only growth matters.
  let grown = 1;
  try {
    const memory = new WebAssembly.Instance(new WebAssembly.Module(buildMemoryModule(byModule))).exports.memory;
    const step = Math.floor(GIB / PAGE);
    for (let target = step; target <= byModule; target += step) {
      memory.grow(BigInt(target - grown));
      grown = target;
    }
    if (byModule > grown) { memory.grow(BigInt(byModule - grown)); grown = byModule; }
  } catch { /* grown holds the last size that worked */ }

  const gib = (pages) => (pages * PAGE / GIB).toFixed(2) + 'GiB';

  return {
    ok: true,
    detail:
      `module-declared=${gib(byModule)} grown=${gib(grown)} ` +
      `js-constructor=${byConstructor ? gib(byConstructor) : 'unsupported'} ` +
      (byConstructor && byConstructor < byModule
        ? '(the constructor cap is lower and is NOT the emulator ceiling)'
        : ''),
  };
}

onmessage = (event) => {
  const { file, origin } = event.data;
  postMessage({
    agent: navigator.userAgent,
    results: [
      { name: 'sync-xhr-responseType', ...probeSyncXhrResponseType() },
      { name: 'sync-xhr-range', ...probeSyncXhrRange(origin) },
      { name: 'filereadersync-large', ...probeFileReaderSync(file) },
      { name: 'wasm-memory64-maximum', ...probeMemory64Maximum() },
    ],
  });
};
