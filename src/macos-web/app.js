'use strict';

// File handles, never contents. A macOS root is gigabytes and the dyld shared cache alone is 5.4 GB, so
// nothing is read here: the worker mounts these through WORKERFS and reads only the slices the guest
// actually touches. It also makes adding a multi-gigabyte root instant rather than a long silent copy.
const files = new Map(); // guest path -> File
const headers = new Map(); // guest path -> first bytes, for the Mach-O badge only
const hosted = new Map(); // guest path -> URL on the local server, read by range and never downloaded

// Guest directory -> the directory on the served root it stands for, for the subtrees where the two are
// not the same path. A bundle is mounted where a launched app would appear -- /Applications/<name>.app,
// the same guest root the native launcher maps a bundle onto -- while on the server it stays wherever
// the system keeps it, which for a shipped app is under /System. Without this the worker resolves a name
// the page did not attach against /Applications/<name>.app on the server, where there is nothing.
const hostedDirs = new Map();
const GUEST_ROOT = '/root';
const SERVED_ROOT = `${location.origin}/macos-root`;
const DEMO_PATH = '/demo/macho_trace_arm64';
const GUI_DEMO_PATH = '/demo/macho_calcdemo_arm64';
const PAINT_DEMO_PATH = '/demo/macho_paintprobe_arm64';
const CALCULATOR_BUNDLE = '/System/Applications/Calculator.app';
let sawSharedCache = false;
let sawGuiBinding = false;

let selected = null;
let running = false;
let moduleFactory = null;

const $ = (id) => document.getElementById(id);

const el = {
  status: $('status'), run: $('run'), clear: $('clear'),
  tree: $('tree'), fsCount: $('fs-count'), drop: $('drop'),
  trace: $('trace'), traceCount: $('trace-count'), filter: $('filter'),
  selPath: $('sel-path'), imageInfo: $('image-info'), resultInfo: $('result-info'),
  modules: $('modules'), modCount: $('mod-count'),
  syscalls: $('syscalls'), sysTotal: $('sys-total'), stop: $('stop'),
  failures: $('failures'), failTotal: $('fail-total'),
  screenPanel: $('screen-panel'), screenCanvas: $('screen-canvas'), screenSize: $('screen-size'),
  screenOverlay: $('screen-overlay'), screenWindows: $('screen-windows'),
  screenStage: $('screen-stage'), screenStream: $('screen-stream'), liveInfo: $('live-info'),
};

/* ------------------------------------------------------------------ status */

function setStatus(text, kind) {
  el.status.textContent = text;
  el.status.className = kind || '';
}

/* -------------------------------------------------------------- Mach-O ID */

// Read enough of the header in JS to label the tree before anything runs. The emulator reports the
// authoritative answer at run time; this is only so the user can tell which upload is the executable.
function identify(bytes) {
  if (bytes.length < 4) return { kind: 'no' };

  const be = (o) => (bytes[o] << 24 | bytes[o + 1] << 16 | bytes[o + 2] << 8 | bytes[o + 3]) >>> 0;
  const le = (o) => (bytes[o] | bytes[o + 1] << 8 | bytes[o + 2] << 16 | bytes[o + 3] << 24) >>> 0;

  const magic = le(0);
  if (magic === 0xfeedfacf || magic === 0xfeedface) {
    return { kind: 'macho', label: magic === 0xfeedfacf ? 'Mach-O 64' : 'Mach-O 32', dynamic: hasDylinker(bytes) };
  }

  const fat = be(0);
  if (fat === 0xcafebabe || fat === 0xcafebabf || fat === 0xbebafeca || fat === 0xbfbafeca) {
    return { kind: 'macho', label: 'fat', dynamic: null };
  }

  return { kind: 'no' };
}

// LC_LOAD_DYLINKER is 0x0e. A thin little-endian 64-bit image is the only shape walked here; anything
// else reports "unknown" rather than guessing, and the run itself will say for certain.
function hasDylinker(bytes) {
  try {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (view.getUint32(0, true) !== 0xfeedfacf) return null;

    const count = view.getUint32(16, true);
    let offset = 32;

    for (let i = 0; i < count; ++i) {
      if (offset + 8 > bytes.length) return null;
      const cmd = view.getUint32(offset, true);
      const size = view.getUint32(offset + 4, true);
      if (size < 8) return null;
      if (cmd === 0x0e) return true;
      offset += size;
    }
    return false;
  } catch {
    return null;
  }
}

/* ----------------------------------------------------------------- upload */

function normalize(path) {
  const parts = String(path).split('/').filter((p) => p && p !== '.');
  return '/' + parts.join('/');
}

async function addFile(path, file) {
  const guest = normalize(path);
  files.set(guest, file);

  // Only the header is read, and only to label the row. Everything else stays on disk.
  try {
    headers.set(guest, new Uint8Array(await file.slice(0, 4096).arrayBuffer()));
  } catch {
    headers.delete(guest);
  }

  if (!selected) select(guest);
  return guest;
}

// A served file is attached by URL and nothing else: no header is read here, because the tree labels a
// served row by where it came from rather than by what is in it, and reading one would defeat the point
// of serving it.
async function addServed(path, source) {
  const guest = normalize(path);
  const size = source.size === undefined ? await headSize(source.url) : source.size;
  hosted.set(guest, { url: source.url, size });

  if (!selected) select(guest);
  return guest;
}

function attachSource(path, source) {
  return source instanceof File ? addFile(path, source) : addServed(path, source);
}

function sourceAt(guest) {
  return files.get(guest) || hosted.get(guest) || null;
}

async function sourceText(source) {
  if (source instanceof File) return source.text();

  const response = await fetch(source.url);
  if (!response.ok) throw new Error(`${response.status} for ${source.url}`);
  return response.text();
}

async function addFromInput(list) {
  const entries = [];
  for (const file of list) {
    // A directory upload carries its relative path; a plain file selection does not.
    const rel = file.webkitRelativePath && file.webkitRelativePath.length ? file.webkitRelativePath : file.name;
    entries.push([rel, file]);
  }
  await absorb(entries);
}

async function addFromDataTransfer(items) {
  const entries = [];

  const walk = async (entry, prefix) => {
    if (entry.isFile) {
      const file = await new Promise((res, rej) => entry.file(res, rej));
      entries.push([prefix + '/' + entry.name, file]);
      return;
    }
    if (entry.isDirectory) {
      const reader = entry.createReader();
      // readEntries returns at most 100 per call, so it has to be drained in a loop.
      for (;;) {
        const batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        if (!batch.length) break;
        for (const child of batch) await walk(child, prefix + '/' + entry.name);
      }
    }
  };

  const roots = [];
  for (const item of items) {
    const entry = item.webkitGetAsEntry ? item.webkitGetAsEntry() : null;
    if (entry) roots.push(entry);
  }

  if (roots.length) {
    for (const entry of roots) await walk(entry, '');
  }

  await absorb(entries);
}

/* ---------------------------------------------------------------- bundles */

// A bundle is a directory, and a picker hands one over as a flat list of files whose relative paths all
// begin inside it. Where that prefix sits is not fixed -- picking Calculator.app gives
// "Calculator.app/Contents/...", picking the folder above it gives "Applications/Calculator.app/..." --
// so the first ".app" component is the root and everything before it is the picker's own accident.
function bundleRootOf(relative) {
  const parts = String(relative).split('/').filter((part) => part && part !== '.');
  const index = parts.findIndex((part) => part.toLowerCase().endsWith('.app'));
  if (index < 0) return null;

  return { name: parts[index], rest: parts.slice(index + 1).join('/') };
}

async function absorb(entries) {
  const mount = normalize($('opt-bundle-mount').value || '/Applications');
  const bundles = new Map();

  for (const [relative, source] of entries) {
    const bundle = bundleRootOf(relative);

    if (!bundle || !bundle.rest) {
      await attachSource(relative, source);
      continue;
    }

    await attachSource(`${mount}/${bundle.name}/${bundle.rest}`, source);

    if (!bundles.has(bundle.name)) bundles.set(bundle.name, new Set());
    bundles.get(bundle.name).add(bundle.rest);
  }

  for (const [name, contents] of bundles) await adoptBundle(mount, name, contents);

  renderTree();
  updateRunButton();
}

// CFBundleExecutable is the only authority on which file inside Contents/MacOS is the program, and a
// modern Info.plist is usually a binary plist, which this does not decode. The fallbacks are named in
// the trace rather than applied quietly, because picking the wrong file looks like the app failing.
async function readBundleExecutable(source) {
  if (!source) return { name: null, format: 'absent' };

  try {
    const text = await sourceText(source);
    if (text.startsWith('bplist')) return { name: null, format: 'binary' };

    const match = /<key>\s*CFBundleExecutable\s*<\/key>\s*<string>([^<]*)<\/string>/.exec(text);
    return { name: match ? match[1].trim() : null, format: 'xml' };
  } catch {
    return { name: null, format: 'unreadable' };
  }
}

async function adoptBundle(mount, name, contents) {
  const guestBundle = `${mount}/${name}`;
  const executables = [...contents].filter((rest) => /^Contents\/MacOS\/[^/]+$/.test(rest));

  const plist = await readBundleExecutable(sourceAt(`${guestBundle}/Contents/Info.plist`));

  let chosen = null;
  let how = '';

  if (plist.name && executables.includes(`Contents/MacOS/${plist.name}`)) {
    chosen = `Contents/MacOS/${plist.name}`;
    how = 'CFBundleExecutable in Contents/Info.plist';
  } else if (executables.length === 1) {
    chosen = executables[0];
    how = `the only file in Contents/MacOS (Info.plist is ${plist.format})`;
  } else {
    const stem = name.replace(/\.app$/i, '');
    const match = executables.find((rest) => rest === `Contents/MacOS/${stem}`);
    if (match) {
      chosen = match;
      how = `the bundle's own name (Info.plist is ${plist.format})`;
    }
  }

  addRow('sysinfo', (row) => {
    row.textContent = `Attached ${name}: ${contents.size} files under ${guestBundle}.`;
  });

  if (!chosen) {
    addRow('err', (row) => {
      row.textContent =
        `Could not tell which binary ${name} launches: Info.plist is ${plist.format} and Contents/MacOS holds ` +
        `${executables.length} file${executables.length === 1 ? '' : 's'}. Pick one in the tree.`;
    });
    return;
  }

  select(`${guestBundle}/${chosen}`);
  $('opt-gui').checked = true;

  addRow('sysinfo', (row) => {
    row.textContent = `Launching ${guestBundle}/${chosen}, chosen by ${how}.`;
  });
}

function select(guest) {
  selected = guest;
  el.selPath.textContent = guest;
  renderTree();
  updateRunButton();
}

function updateRunButton() {
  el.run.disabled = running || !moduleFactory || !selected;
}

/* ------------------------------------------------------------------- tree */

function buildTree() {
  const root = { dirs: new Map(), files: [] };

  const everything = new Map(files);
  for (const [path, entry] of hosted) {
    if (!everything.has(path)) everything.set(path, { size: entry.size, hosted: true });
  }

  for (const [path, file] of everything) {
    const parts = path.split('/').filter(Boolean);
    let node = root;
    for (let i = 0; i < parts.length - 1; ++i) {
      if (!node.dirs.has(parts[i])) node.dirs.set(parts[i], { dirs: new Map(), files: [] });
      node = node.dirs.get(parts[i]);
    }
    node.files.push({ name: parts[parts.length - 1], path, size: file.size, hosted: Boolean(file.hosted) });
  }

  return root;
}

function renderTree() {
  el.tree.textContent = '';
  const total = files.size + hosted.size;
  el.fsCount.textContent = total ? `${total} file${total === 1 ? '' : 's'}` : '';

  if (!total) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = 'Nothing uploaded. The demo binary is a good first run.';
    el.tree.appendChild(empty);
    return;
  }

  const emitDir = (node, depth) => {
    for (const [name, child] of [...node.dirs].sort((a, b) => a[0].localeCompare(b[0]))) {
      const row = document.createElement('div');
      row.className = 'node dir';
      row.style.paddingLeft = `${depth * 12 + 4}px`;
      row.textContent = name + '/';
      el.tree.appendChild(row);
      emitDir(child, depth + 1);
    }

    for (const file of node.files.sort((a, b) => a.name.localeCompare(b.name))) {
      const id = file.hosted ? { kind: 'hosted' } : identify(headers.get(file.path) || new Uint8Array(0));
      const row = document.createElement('div');
      row.className = 'node file' + (file.path === selected ? ' selected' : '');
      row.style.paddingLeft = `${depth * 12 + 4}px`;
      row.title = `${file.path} (click to select as the executable)`;

      const name = document.createElement('span');
      name.className = 'nm';
      name.textContent = file.name;
      row.appendChild(name);

      const badge = document.createElement('span');
      if (id.kind === 'hosted') {
        badge.className = 'badge';
        badge.textContent = 'served';
      } else if (id.kind === 'macho') {
        badge.className = 'badge macho';
        badge.textContent = id.dynamic === true ? 'dynamic' : id.label;
        if (id.dynamic === true) badge.className = 'badge dyn';
      } else {
        badge.className = 'badge no';
        badge.textContent = 'data';
      }
      row.appendChild(badge);

      const size = document.createElement('span');
      size.className = 'sz';
      size.textContent = bytes(file.size);
      row.appendChild(size);

      const remove = document.createElement('button');
      remove.className = 'rm';
      remove.textContent = '×';
      remove.title = 'Remove';
      remove.addEventListener('click', (event) => {
        event.stopPropagation();
        files.delete(file.path);
        headers.delete(file.path);
        hosted.delete(file.path);
        if (selected === file.path) { selected = null; el.selPath.textContent = '—'; }
        renderTree();
        updateRunButton();
      });
      row.appendChild(remove);

      row.addEventListener('click', () => select(file.path));
      el.tree.appendChild(row);
    }
  };

  emitDir(buildTree(), 0);
}

function bytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1048576).toFixed(1)} MiB`;
  return `${(n / 1073741824).toFixed(2)} GiB`;
}

/* ------------------------------------------------------------------ trace */

let traceRows = 0;
let pending = [];
let flushScheduled = false;

function addRow(className, build) {
  const row = document.createElement('div');
  row.className = 'row ' + className;
  build(row);
  pending.push(row);
  ++traceRows;

  if (!flushScheduled) {
    flushScheduled = true;
    requestAnimationFrame(flushRows);
  }
}

// Batched through requestAnimationFrame: a run emits thousands of events and appending each one
// synchronously makes the page unusable long before the guest finishes. Only the new rows are matched
// against the filter here -- re-scanning the whole trace every frame is quadratic in its length, which
// is exactly the case this view exists to handle.
function flushRows() {
  flushScheduled = false;
  if (!pending.length) return;

  const fragment = document.createDocumentFragment();
  for (const row of pending) fragment.appendChild(row);
  pending = [];


  const scroll = el.trace.parentElement;
  const atBottom = scroll.scrollTop + scroll.clientHeight >= scroll.scrollHeight - 40;
  el.trace.appendChild(fragment);
  if (atBottom) scroll.scrollTop = scroll.scrollHeight;

  el.traceCount.textContent = `${traceRows} line${traceRows === 1 ? '' : 's'}`;
  renderSyscallCounts();

  // Applied to the whole trace rather than just the new rows: a group's visibility depends on the call
  // above it, which may already have been appended in an earlier frame.
  if (el.filter.value.trim() || $('only-failed').checked || enabledKinds().size < 6) applyFilter();
}


// A run makes thousands of calls and the interesting thing is usually which ones, not the order. Counted
// here rather than derived from the trace, because the trace can be filtered or truncated.
const syscallCounts = new Map();
let syscallTotal = 0;
let countsDirty = false;

function countSyscall(name) {
  syscallCounts.set(name, (syscallCounts.get(name) || 0) + 1);
  ++syscallTotal;
  countsDirty = true;
}

const failureCounts = new Map();
let failureTotal = 0;

function countFailure(name) {
  failureCounts.set(name, (failureCounts.get(name) || 0) + 1);
  ++failureTotal;
  countsDirty = true;
}

// Bars rather than numbers: the shape of a run is what a reader takes in first, and a hundred calls next
// to three is a fact you should not have to read digits to notice.
function renderBars(target, counts, limit, bad) {
  target.textContent = '';

  const rows = [...counts].sort((a, b) => b[1] - a[1]).slice(0, limit);
  if (!rows.length) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = bad ? 'none' : '—';
    target.appendChild(empty);
    return;
  }

  const largest = rows[0][1];
  for (const [name, count] of rows) {
    const line = document.createElement('div');

    const track = document.createElement('div');
    track.className = 'track';

    const fill = document.createElement('div');
    fill.className = bad ? 'fill bad' : 'fill';
    fill.style.width = `${Math.max(2, (count / largest) * 100)}%`;

    const label = document.createElement('span');
    label.className = 'label';
    label.textContent = name;

    track.append(fill, label);

    const n = document.createElement('span');
    n.className = 'n';
    n.textContent = String(count);

    line.append(track, n);
    target.appendChild(line);
  }
}

function renderSyscallCounts() {
  if (!countsDirty) return;
  countsDirty = false;

  el.sysTotal.textContent = syscallTotal ? String(syscallTotal) : '';
  el.failTotal.textContent = failureTotal ? String(failureTotal) : '';

  renderBars(el.syscalls, syscallCounts, 16, false);
  renderBars(el.failures, failureCounts, 8, true);
}


// Which kind of call this is, for colour. Prefix matching rather than a table of every name: the naming
// is consistent enough that a new syscall lands in the right group without anyone remembering to add it,
// and the cost of a wrong guess is a colour.
function categorise(name) {
  if (/^(_kernelrpc|mach_|task_|thread_|host_|semaphore|bootstrap)/.test(name)) return 'mach';
  if (/(mmap|munmap|mprotect|madvise|shared_region|vm_|brk|mincore)/.test(name)) return 'memory';
  if (/(open|close|read|write|stat|lseek|access|fcntl|dup|getdirent|unlink|rename|mkdir|rmdir|link|attrlist|fsgetpath|pread|pwrite|ioctl)/.test(name))
    return 'file';
  if (/(sig|kill|abort)/.test(name)) return 'signal';
  if (/(pid|uid|gid|proc|exec|exit|fork|thread|audit|rlimit|sysctl|csrctl|mac_syscall)/.test(name)) return 'process';
  return 'other';
}

// Every call and its decoded arguments form one collapsible group. A trace of thousands of lines is
// unreadable as a flat list, and the arguments are what matters only once a call looks interesting.
let currentGroup = null;
let syscallIndex = 0;

function beginSyscall(event) {
  const kind = categorise(event.name);
  ++syscallIndex;

  const details = [];
  currentGroup = { details, failed: false };

  addRow(`sys k-${kind}`, (row) => {
    const bar = document.createElement('span'); bar.className = 'kind';
    const caret = document.createElement('span'); caret.className = 'caret'; caret.textContent = '▸';
    const idx = document.createElement('span'); idx.className = 'idx'; idx.textContent = String(syscallIndex);
    const nm = document.createElement('span'); nm.className = 'nm'; nm.textContent = event.name;
    const num = document.createElement('span'); num.className = 'pc';
    num.textContent = `0x${Number(event.id).toString(16)}`;
    const mod = document.createElement('span'); mod.className = 'mod'; mod.textContent = event.pc;

    row.append(bar, caret, idx, nm, num, mod);
    currentGroup.header = row;
    currentGroup.caret = caret;

    row.addEventListener('click', () => {
      const collapsed = details.length > 0 && !details[0].classList.contains('collapsed');
      for (const d of details) d.classList.toggle('collapsed', collapsed);
      caret.textContent = collapsed ? '▸' : '▾';
    });
  });
}

function attachDetail(className, build) {
  addRow(className, (row) => {
    build(row);
    // Collapsed by default: the call is the signal, the arguments are the follow-up.
    row.classList.add('collapsed');
    if (currentGroup) currentGroup.details.push(row);
  });
}

function clearTrace() {
  syscallCounts.clear();
  failureCounts.clear();
  syscallTotal = 0;
  failureTotal = 0;
  syscallIndex = 0;
  currentGroup = null;
  countsDirty = true;
  renderSyscallCounts();

  sawSharedCache = false;
  sawGuiBinding = false;
  el.screenPanel.hidden = true;
  el.screenCanvas.width = 1;
  el.screenCanvas.height = 1;
  el.screenSize.textContent = '';
  el.screenOverlay.replaceChildren();
  el.screenWindows.replaceChildren();
  resetStream();

  el.trace.textContent = '';
  pending = [];
  traceRows = 0;
  el.traceCount.textContent = '';
  el.modules.textContent = '';
  el.modCount.textContent = '';
  const empty = document.createElement('div');
  empty.className = 'empty';
  empty.textContent = '—';
  el.modules.appendChild(empty);
}

function enabledKinds() {
  const set = new Set();
  for (const box of document.querySelectorAll('#kinds input[data-kind]')) {
    if (box.checked) set.add(box.dataset.kind);
  }
  return set;
}

// A row is hidden if its text misses the needle, its category is switched off, or "failed only" is on and
// it belongs to a call that worked. Detail rows follow their own call so a group never half-disappears.
function rowIsVisible(row, needle, kinds, failedOnly) {
  if (needle && !row.textContent.toLowerCase().includes(needle)) return false;

  const kind = [...row.classList].find((c) => c.startsWith('k-'));
  if (kind && !kinds.has(kind.slice(2))) return false;

  if (failedOnly && row.classList.contains('sys') && !row.classList.contains('failed')) return false;

  return true;
}

function applyFilter() {
  const needle = el.filter.value.trim().toLowerCase();
  const kinds = enabledKinds();
  const failedOnly = $('only-failed').checked;

  let visibleGroup = true;
  for (const row of el.trace.children) {
    if (row.classList.contains('sys')) {
      visibleGroup = rowIsVisible(row, needle, kinds, failedOnly);
      row.classList.toggle('hidden', !visibleGroup);
      continue;
    }

    // Details and failures belong to the call above them.
    if (row.classList.contains('det') || row.classList.contains('fail')) {
      row.classList.toggle('hidden', !visibleGroup);
      continue;
    }

    row.classList.toggle('hidden', Boolean(needle) && !row.textContent.toLowerCase().includes(needle));
  }
}

function kvTable(target, pairs) {
  target.textContent = '';
  if (!pairs.length) {
    const row = target.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 2;
    cell.className = 'empty';
    cell.textContent = '—';
    return;
  }
  for (const [key, value] of pairs) {
    const row = target.insertRow();
    row.insertCell().textContent = key;
    row.insertCell().textContent = value;
  }
}

/* ------------------------------------------------------------------ events */

let moduleCount = 0;

function handleEvent(event) {
  switch (event.t) {
    case 'started':
      addRow('sysinfo', (row) => { row.textContent = `sogen ${event.backend} — launching ${event.exe}`; });
      break;

    case 'image': {
      const pairs = [];
      if (!Number(event.readable)) {
        pairs.push(['readable', 'no']);
      } else {
        pairs.push(['size', bytes(Number(event.bytes || 0))]);
        pairs.push(['abi', Number(event.arm64e) ? 'arm64e' : 'arm64']);
        pairs.push(['entry', event.entry || '—']);
        pairs.push(['dylinker', event.dylinker ? event.dylinker : 'none (static)']);
      }
      if (event.error) pairs.push(['error', event.error]);
      kvTable(el.imageInfo, pairs);

      if (event.dylinker) {
        const dylinkerPresent = files.has(event.dylinker) || hosted.has(event.dylinker);
        const cachePresent =
          [...files.keys()].some((p) => p.includes('dyld_shared_cache')) ||
          [...hosted.keys()].some((p) => p.includes('dyld_shared_cache'));

        if (dylinkerPresent && cachePresent) {
          addRow('sysinfo', (row) => {
            row.textContent =
              `Dynamically linked against ${event.dylinker}. The dylinker and the shared cache are ` +
              `mounted, and the cache is read a chunk at a time as the guest reaches it.`;
          });
        } else {
          const missing = [];
          if (!dylinkerPresent) missing.push(event.dylinker);
          if (!cachePresent) missing.push('a dyld_shared_cache_arm64e* set');

          addRow('err', (row) => {
            row.textContent =
              `This image is dynamically linked against ${event.dylinker}, and ${missing.join(' and ')} ` +
              `${missing.length > 1 ? 'are' : 'is'} not loaded.`;
          });
          addRow('err', (row) => {
            row.textContent =
              '  Press "Load macOS root". The server serves it and the page reads ranges from it; nothing ' +
              'is downloaded or picked.';
          });
          addRow('sysinfo', (row) => {
            row.textContent =
              '  Start the server with --macos-root pointing at a directory holding usr/lib/dyld and ' +
              'System/Library/dyld. Symlinks are fine there -- the server resolves them, which a browser ' +
              'folder picker cannot.';
          });
        }
      }
      break;
    }

    case 'module':
      ++moduleCount;
      if (moduleCount === 1) el.modules.textContent = '';
      el.modCount.textContent = String(moduleCount);
      {
        const line = document.createElement('div');
        const name = document.createElement('span');
        name.textContent = event.name;
        name.title = event.name;
        const at = document.createElement('span');
        at.textContent = event.base;
        line.appendChild(name);
        line.appendChild(at);
        el.modules.appendChild(line);
      }
      addRow('sysinfo', (row) => { row.textContent = `Mapped ${event.name} at ${event.base} (${bytes(Number(event.size))})`; });
      break;

    case 'syscall':
      countSyscall(event.name);
      beginSyscall(event);
      break;

    case 'detail':
      attachDetail('det', (row) => {
        if (event.label) {
          const key = document.createElement('span');
          key.className = 'k';
          key.textContent = `${event.label}: `;
          row.appendChild(key);
        }
        row.appendChild(document.createTextNode(event.value));
      });
      break;

    case 'failed':
      countFailure(event.name || `errno ${event.errno}`);

      // The failure marks the call itself, not just a line under it: a reader scanning a collapsed trace
      // has to be able to see which calls did not work without expanding any of them.
      if (currentGroup && currentGroup.header) {
        currentGroup.header.classList.add('failed');
        currentGroup.failed = true;
      }

      attachDetail('fail', (row) => {
        row.textContent = event.name ? `Failed: ${event.name} (${event.errno})` : `Failed: errno ${event.errno}`;
      });
      break;

    case 'stdout':
      addRow('out', (row) => { row.textContent = event.text; });
      break;

    // The emulator's own logger, routed here by --log rather than to a terminal it has no colours for.
    // The level is the colour it was printed in; only red and yellow read as something gone wrong.
    case 'log':
      addRow(event.level === 'error' || event.level === 'warn' ? 'err' : 'sysinfo', (row) => { row.textContent = event.text; });
      break;

    case 'stderr':
      addRow('err', (row) => { row.textContent = event.text; });
      break;

    case 'memory':
      window.__lastMemory = event;
      break;

    case 'counters':
      window.__lastCounters = event;
      break;

    case 'dyld-error':
      addRow('fatal', (row) => { row.textContent = `dyld refused the launch: ${event.message}`; });

      // A library that lives only inside the shared cache cannot be found on disk, so when the cache
      // never mapped that is the cause rather than a missing file, and it is worth saying outright.
      if (!sawSharedCache) {
        addRow('fatal', (row) => {
          row.textContent =
            '    The shared cache was never mapped, so anything inside it -- SkyLight, AppKit, ' +
            'CoreGraphics -- cannot be found. Load the macOS root before running a dynamic binary.';
        });
      }
      break;

    // Absent entirely when dyld never mapped the cache, which is the failure that looks like nothing at
    // all until a library that lives only inside it fails to load.
    case 'cache':
      sawSharedCache = true;
      addRow('sysinfo', (row) => {
        row.textContent = `Shared cache mapped: ${event.mappings} mappings, ${event.rebased} pointers rebased.`;
      });
      break;

    case 'gui':
      sawGuiBinding = true;
      addRow(Number(event.bound) > 0 ? 'sysinfo' : 'fatal', (row) => {
        row.textContent =
          `Window server: ${event.bound} of ${event.registered} routines intercepted` +
          (Number(event.unbound) ? `, ${event.unbound} not exported on this system` : '') +
          (Number(event.bound) ? '.' : ' -- the guest will reach the real SkyLight, which is not there.');
      });
      break;

    case 'stream':
      stream.attached = Boolean(Number(event.attached));
      if (stream.attached) {
        addRow('sysinfo', (row) => {
          row.textContent = `Streaming the composed desktop every ${event.interval} ms while the guest runs.`;
        });
      } else {
        addRow('fatal', (row) => { row.textContent = `Frames are not being streamed: ${event.reason}`; });
      }
      renderStream();
      break;

    case 'status':
      stream.sent = Number(event.frames) || 0;
      stream.dropped = Number(event.dropped) || 0;
      stream.presents = Number(event.presents) || 0;
      stream.guestWindows = Number(event.windows) || 0;
      stream.threads = Number(event.threads) || 0;
      stream.inputDelivered = Number(event.input) || 0;
      stream.syscalls = Number(event.syscalls) || 0;
      if (event.credit !== undefined) stream.credit = Number(event.credit);
      renderStream();
      break;

    // Only the first event of each kind is listed: a few seconds of mouse movement is thousands of
    // identical rows, and the counts in Live are the part that keeps changing.
    case 'input': {
      if (Number(event.delivered)) break;

      if (reportedInput.has(event.message)) break;
      reportedInput.add(event.message);

      const code = Number(event.code) || 0;
      const lparam = Number(event.lparam) || 0;
      const where = code >= WM_MOUSEMOVE ? `at ${lparam & 0xFFFF},${(lparam >>> 16) & 0xFFFF}` : `vk ${Number(event.wparam)}`;

      addRow('err', (row) => {
        row.textContent =
          `${event.message} ${where} reached the emulator and no guest-side path carried it further. ` +
          `Further undelivered ${event.message} events are counted in Live rather than listed.`;
      });
      break;
    }

    // A guest that aborts writes nothing anywhere; the frames are the only account of what happened.
    case 'frame':
      addRow('err', (row) => { row.textContent = `    ${event.text}`; });
      break;

    case 'exited':
      window.__lastExit = { status: Number(event.status) | 0 };
      stream.stopped = `exited ${Number(event.status) | 0}`;
      renderStream();
      break;

    case 'stopped':
      window.__lastExit = { stopped: event.reason || 'stopped', detail: event.detail, pc: event.pc };
      stream.stopped = event.reason || 'stopped';
      renderStream();
      addRow('err', (row) => {
        const detail = event.detail ? ` — ${event.detail}` : '';
        row.textContent = `Stopped: ${event.reason || 'unknown'}${detail} (pc ${event.pc || '?'})`;
      });
      break;

    // Not a stop: the guest is parked and the emulator is polling the page for input. Headlessly this is
    // where the run would have ended, so it is worth saying out loud that it did not.
    case 'idle':
      stream.idle = true;
      renderStream();
      addRow('sysinfo', (row) => { row.textContent = `${event.reason}. Click the desktop above.`; });
      break;

    case 'fatal':
      addRow('fatal', (row) => { row.textContent = `failed: ${event.message}`; });
      window.__lastExit = { fatal: event.message };
      break;

    default:
      break;
  }
}

// The composed desktop, with the window rectangles the guest asked for drawn over it. The overlay is
// what makes an all-black frame readable: it separates "no window was created" from "a window was
// created and nothing drew into it".
//
// The pixels arrive as a transferred RGBA buffer rather than an encoded image: at a screenshot's size a
// PNG is already the size of its pixels (the encoder emits stored deflate blocks), base64 adds a third
// on top, and a decode per frame buys nothing a putImageData does not already do.
function drawFrame(frame) {
  const width = Number(frame.width) || 0;
  const height = Number(frame.height) || 0;
  const expected = width * height * 4;
  if (!width || !height || !frame.pixels || frame.pixels.byteLength !== expected) return;

  el.screenPanel.hidden = false;
  el.screenSize.textContent = `${width}x${height}`;

  const canvas = el.screenCanvas;
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }

  desktop.width = width;
  desktop.height = height;

  // alpha:false so a translucent window shows what the compositor put behind it rather than the page.
  const context = canvas.getContext('2d', { alpha: false });
  const pixels = new Uint8ClampedArray(frame.pixels.buffer, frame.pixels.byteOffset, expected);
  context.putImageData(new ImageData(pixels, width, height), 0, 0);

  const windows = Array.isArray(frame.windows) ? frame.windows : [];
  desktop.windows = windows;

  el.screenOverlay.replaceChildren(...windows.map((w) => {
    const box = document.createElement('div');
    box.className = w.visible ? 'box' : 'box hidden-window';
    box.style.left = `${(w.x / width) * 100}%`;
    box.style.top = `${(w.y / height) * 100}%`;
    box.style.width = `${(w.w / width) * 100}%`;
    box.style.height = `${(w.h / height) * 100}%`;

    const tag = document.createElement('span');
    tag.textContent = `#${w.id}`;
    box.appendChild(tag);
    return box;
  }));

  if (!windows.length) {
    el.screenWindows.replaceChildren(Object.assign(document.createElement('div'),
      { className: 'empty', textContent: 'The guest created no windows.' }));
    return;
  }

  el.screenWindows.replaceChildren(...windows.map((w) => {
    const row = document.createElement('div');
    row.className = w.visible ? 'row' : 'row off';

    const id = document.createElement('span');
    id.textContent = `#${w.id}`;

    const geometry = document.createElement('span');
    geometry.textContent = `${w.w}x${w.h} at ${w.x},${w.y}`;

    const state = document.createElement('span');
    state.className = 'tag';
    state.textContent = w.visible ? `level ${w.level}` : 'not ordered in';

    row.append(id, geometry, state);
    return row;
  }));
}

/* ------------------------------------------------------------------ stream */

const desktop = { width: 0, height: 0, windows: [] };
const reportedInput = new Set();

const stream = {
  drawn: 0, sent: 0, dropped: 0, presents: 0, threads: 0, guestWindows: 0,
  inputSent: 0, inputDelivered: 0, inputDropped: 0, unmappedKeys: 0,
  fps: 0, lastAt: 0, attached: null, stopped: null, credit: null, idle: false, syscalls: 0,
};

window.__stream = stream;

// The composed desktop's geometry, for anything driving the page from outside: where a guest window sits
// is the only way a script can aim a click at one rather than at the desktop behind it.
window.__desktop = desktop;

// Where each guest path is served from. Exposed for the same reason __stream and __desktop are: an
// out-of-process check has no other way to ask what the page attached.
window.__hostedMap = hosted;

function resetStream() {
  Object.assign(stream, {
    drawn: 0, sent: 0, dropped: 0, presents: 0, threads: 0, guestWindows: 0,
    inputSent: 0, inputDelivered: 0, inputDropped: 0, unmappedKeys: 0,
    fps: 0, lastAt: 0, attached: null, stopped: null, credit: null, idle: false, syscalls: 0,
  });
  desktop.width = 0;
  desktop.height = 0;
  desktop.windows = [];
  reportedInput.clear();
  renderStream();
}

// Two views of the same numbers, because they answer different questions: the strip under the canvas
// tells you at a glance whether pixels are still arriving, and the table keeps every counter the run
// produced so a wedged guest can be told apart from a finished one after the fact.
function renderStream() {
  const parts = [
    ['frames', String(stream.drawn)],
    ['fps', stream.fps ? stream.fps.toFixed(1) : '—'],
    ['presents', String(stream.presents)],
    ['threads', String(stream.threads)],
    ['input', `${stream.inputSent} sent`],
  ];

  if (stream.idle) parts.push(['guest', 'waiting for input']);
  if (stream.dropped) parts.push(['refused', String(stream.dropped)]);
  if (stream.stopped) parts.push(['stopped', stream.stopped]);

  el.screenStream.replaceChildren(...parts.map(([key, value]) => {
    const span = document.createElement('span');
    span.textContent = `${key} `;
    const strong = document.createElement('b');
    if (key === 'refused' || key === 'stopped') strong.className = 'warn';
    strong.textContent = value;
    span.appendChild(strong);
    return span;
  }));

  const rows = [
    ['frames drawn', String(stream.drawn)],
    ['frames sent', String(stream.sent)],
    ['frames refused', String(stream.dropped)],
    ['frame rate', stream.fps ? `${stream.fps.toFixed(1)} /s` : '—'],
    ['present count', String(stream.presents)],
    ['guest windows', String(stream.guestWindows)],
    ['guest threads', String(stream.threads)],
    ['input sent', String(stream.inputSent)],
    ['input reached guest side', String(stream.inputDelivered)],
    // Counted by the module rather than by the trace, so it is still there when the trace is off --
    // which is how a long run is made, and the only run whose rate is worth knowing.
    ['guest syscalls', String(stream.syscalls)],
  ];

  if (stream.credit !== null) rows.push(['lowest page credit', String(stream.credit)]);
  if (stream.inputDropped) rows.push(['input dropped', String(stream.inputDropped)]);
  if (stream.unmappedKeys) rows.push(['keys with no VK', String(stream.unmappedKeys)]);
  if (stream.attached === false) rows.push(['stream', 'not attached']);
  if (stream.stopped) rows.push(['stop reason', stream.stopped]);

  kvTable(el.liveInfo, rows);
}

/* ------------------------------------------------------------------- input */

// Win32 message numbers, because that is what a ui_event carries end to end: the emulator's UI layer is
// shared with the Windows guest path and the macOS side translates out of it, not into it.
const WM_MOUSEMOVE = 0x0200;
const WM_LBUTTONDOWN = 0x0201;
const WM_LBUTTONUP = 0x0202;
const WM_RBUTTONDOWN = 0x0204;
const WM_RBUTTONUP = 0x0205;
const WM_MBUTTONDOWN = 0x0207;
const WM_MBUTTONUP = 0x0208;
const WM_MOUSEWHEEL = 0x020A;
const WM_MOUSEHWHEEL = 0x020E;
const WM_KEYDOWN = 0x0100;
const WM_KEYUP = 0x0101;
const WM_CHAR = 0x0102;
const WM_SYSKEYDOWN = 0x0104;
const WM_SYSKEYUP = 0x0105;

const MK_LBUTTON = 0x0001;
const MK_RBUTTON = 0x0002;
const MK_SHIFT = 0x0004;
const MK_CONTROL = 0x0008;
const MK_MBUTTON = 0x0010;

const WHEEL_DELTA = 120;

const VK_BY_CODE = {
  Escape: 0x1B, Enter: 0x0D, NumpadEnter: 0x0D, Backspace: 0x08, Tab: 0x09, Space: 0x20,
  ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28,
  Delete: 0x2E, Insert: 0x2D, Home: 0x24, End: 0x23, PageUp: 0x21, PageDown: 0x22,
  ShiftLeft: 0x10, ShiftRight: 0x10, ControlLeft: 0x11, ControlRight: 0x11,
  AltLeft: 0x12, AltRight: 0x12, MetaLeft: 0x5B, MetaRight: 0x5C, CapsLock: 0x14,
  Semicolon: 0xBA, Equal: 0xBB, Comma: 0xBC, Minus: 0xBD, Period: 0xBE, Slash: 0xBF,
  Backquote: 0xC0, BracketLeft: 0xDB, Backslash: 0xDC, BracketRight: 0xDD, Quote: 0xDE,
  NumpadMultiply: 0x6A, NumpadAdd: 0x6B, NumpadSubtract: 0x6D, NumpadDecimal: 0x6E, NumpadDivide: 0x6F,
};

function virtualKey(code) {
  if (VK_BY_CODE[code] !== undefined) return VK_BY_CODE[code];
  if (/^Key[A-Z]$/.test(code)) return code.charCodeAt(3);
  if (/^Digit[0-9]$/.test(code)) return code.charCodeAt(5);
  if (/^Numpad[0-9]$/.test(code)) return 0x60 + (code.charCodeAt(6) - 0x30);
  if (/^F([1-9]|1[0-2])$/.test(code)) return 0x6F + Number(code.slice(1));
  return 0;
}

function modifierKeys(event) {
  let keys = 0;
  if (event.shiftKey) keys |= MK_SHIFT;
  if (event.ctrlKey) keys |= MK_CONTROL;
  if (typeof event.buttons === 'number') {
    if (event.buttons & 1) keys |= MK_LBUTTON;
    if (event.buttons & 2) keys |= MK_RBUTTON;
    if (event.buttons & 4) keys |= MK_MBUTTON;
  }
  return keys;
}

function packPoint(x, y) {
  return (((y & 0xFFFF) << 16) | (x & 0xFFFF)) >>> 0;
}

// Desktop coordinates, not CSS ones: the canvas is scaled to fit the panel and the guest only ever knew
// about the desktop it was given.
function desktopPoint(event) {
  const box = el.screenCanvas.getBoundingClientRect();
  if (!box.width || !box.height || !desktop.width || !desktop.height) return null;

  const x = Math.round(((event.clientX - box.left) / box.width) * desktop.width);
  const y = Math.round(((event.clientY - box.top) / box.height) * desktop.height);

  return {
    x: Math.max(0, Math.min(desktop.width - 1, x)),
    y: Math.max(0, Math.min(desktop.height - 1, y)),
  };
}

// The topmost visible window covering the point, so a guest that eventually reads the window field gets
// the one it would have hit. Zero means the desktop itself.
function windowAt(point) {
  let best = 0;
  let bestLevel = -Infinity;

  for (const w of desktop.windows) {
    if (!w.visible) continue;
    if (point && (point.x < w.x || point.y < w.y || point.x >= w.x + w.w || point.y >= w.y + w.h)) continue;
    if (w.level >= bestLevel) {
      bestLevel = w.level;
      best = w.id;
    }
  }

  return best;
}

function sendInput(target, message, wParam, lParam) {
  if (!running || !worker || !$('opt-input').checked) return;

  worker.postMessage({ t: 'input', event: { window: target, message, wParam, lParam } });
  ++stream.inputSent;
}

// lParam is the point in the TARGET WINDOW's client space, not the desktop's: the guest side adds the
// window origin back, the same way sdl_ui_backend::map_window_point does. Sending the desktop point
// here lands every click at twice its offset from the window.
function sendMouse(event, message, wParam) {
  const point = desktopPoint(event);
  if (!point) return;

  const target = windowAt(point);
  const window = target ? desktop.windows.find((w) => w.id === target) : null;
  const x = window ? point.x - window.x : point.x;
  const y = window ? point.y - window.y : point.y;

  sendInput(target, message, wParam, packPoint(x, y));
}

function installInputBridge() {
  const stage = el.screenStage;

  stage.addEventListener('pointerdown', (event) => {
    stage.focus();
    if (!running || !$('opt-input').checked) return;
    event.preventDefault();
    stage.setPointerCapture(event.pointerId);

    const down = [WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN][event.button];
    if (down === undefined) return;
    sendMouse(event, down, modifierKeys(event));
  });

  stage.addEventListener('pointerup', (event) => {
    if (stage.hasPointerCapture(event.pointerId)) stage.releasePointerCapture(event.pointerId);

    const up = [WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP][event.button];
    if (up === undefined) return;
    sendMouse(event, up, modifierKeys(event));
  });

  stage.addEventListener('pointermove', (event) => sendMouse(event, WM_MOUSEMOVE, modifierKeys(event)));

  stage.addEventListener('wheel', (event) => {
    if (!running || !$('opt-input').checked) return;
    event.preventDefault();

    const vertical = Math.round((-event.deltaY / 100) * WHEEL_DELTA);
    const horizontal = Math.round((event.deltaX / 100) * WHEEL_DELTA);

    if (vertical) sendMouse(event, WM_MOUSEWHEEL, (((vertical & 0xFFFF) << 16) | modifierKeys(event)) >>> 0);
    if (horizontal) sendMouse(event, WM_MOUSEHWHEEL, (((horizontal & 0xFFFF) << 16) | modifierKeys(event)) >>> 0);
  }, { passive: false });

  // Shift, control, alt and command reach the guest as their own key events, which is how a Win32
  // consumer is expected to track modifier state: only alt is in the message, as the lParam context bit
  // and the SYSKEY variant, exactly as the SDL backend sends it.
  const key = (event, down) => {
    if (!running || !$('opt-input').checked) return;
    event.preventDefault();

    const vk = virtualKey(event.code);
    if (!vk) {
      // Named rather than dropped: an unmapped key is a hole in this table, and a guest that ignores a
      // keystroke looks exactly the same as one that was never sent it.
      ++stream.unmappedKeys;
      addRow('err', (row) => { row.textContent = `No virtual-key code for ${event.code}; the keystroke was not sent.`; });
      renderStream();
      return;
    }

    const message = event.altKey ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP);
    const lParam = ((event.repeat ? 1 : 0) | (event.altKey ? 1 << 29 : 0) | (down ? 0 : 1 << 31)) >>> 0;

    // No point to hit-test against, so the frontmost window stands in for keyboard focus.
    const focused = windowAt(null);
    sendInput(focused, message, vk, lParam);

    if (down && event.key.length === 1) {
      sendInput(focused, WM_CHAR, event.key.codePointAt(0), lParam);
    }
  };

  stage.addEventListener('keydown', (event) => key(event, true));
  stage.addEventListener('keyup', (event) => key(event, false));

  stage.addEventListener('contextmenu', (event) => {
    if (running && $('opt-input').checked) event.preventDefault();
  });
}

function dispatchLine(line, isError) {
  const trimmed = line.trim();
  if (!trimmed.length) return;

  if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
    try {
      handleEvent(JSON.parse(trimmed));
      return;
    } catch {
      // fall through and show it verbatim
    }
  }

  addRow(isError ? 'err' : 'sysinfo', (row) => { row.textContent = trimmed; });
}

/* -------------------------------------------------------------------- run */

function buildArguments() {
  const args = [`--exe=${selected}`, `--root=${GUEST_ROOT}`];

  const guestArgv = $('opt-argv').value.trim();
  args.push(`--arg=${selected}`);
  if (guestArgv) for (const piece of guestArgv.split(/\s+/)) args.push(`--arg=${piece}`);

  const env = $('opt-env').value.trim();
  if (env) for (const piece of env.split(/[\n,]+/)) { const v = piece.trim(); if (v) args.push(`--env=${v}`); }

  const ignore = $('opt-ignore').value.trim();
  if (ignore) args.push(`--ignore=${ignore}`);

  args.push(`--max-instructions=${Math.max(0, Number($('opt-maxinsn').value) || 0)}`);
  args.push(`--string-limit=${Math.max(0, Number($('opt-strlimit').value) || 0)}`);
  args.push(`--buffer-limit=${Math.max(0, Number($('opt-buflimit').value) || 0)}`);

  if (!$('opt-decode').checked) args.push('--no-decode');
  if (!$('opt-syscalls').checked) args.push('--no-syscalls');
  if (!$('opt-modules').checked) args.push('--no-modules');
  if (!$('opt-memory').checked) args.push('--no-memory-report');
  if ($('opt-log').checked) args.push('--log');

  if ($('opt-gui').checked) {
    args.push('--gui');
    const size = ($('opt-desktop').value || '').trim();
    if (/^\d+\s*[xX]\s*\d+$/.test(size)) args.push(`--desktop-size=${size.replace(/\s+/g, '')}`);
    args.push(`--frame-interval=${Math.max(0, Number($('opt-frame-interval').value) || 0)}`);
  }

  return args;
}

let worker = null;

function ensureWorker() {
  if (worker) return worker;

  worker = new Worker('./emulator-worker.js', { type: 'module' });
  worker.onmessage = (message) => {
    const data = message.data;

    if (data.t === 'event') { handleEvent(data.event); return; }
    if (data.t === 'line') { dispatchLine(data.text, data.isError); return; }

    // Acked whatever happens to the pixels: the module composes nothing while the page is behind, so a
    // frame that failed to draw and was never acked would stop the stream for good.
    if (data.t === 'frame') {
      try {
        drawFrame(data);
        ++stream.drawn;
        const now = performance.now();
        if (stream.lastAt) stream.fps = 1000 / Math.max(1, now - stream.lastAt);
        stream.lastAt = now;
        renderStream();
      } finally {
        if (worker) worker.postMessage({ t: 'frame-ack' });
      }
      return;
    }

    if (data.t === 'input-dropped') { stream.inputDropped = Number(data.count) || 0; renderStream(); return; }
    if (data.t === 'bridge-error') {
      addRow('fatal', (row) => {
        row.textContent = `range read failed at ${data.path} +${data.offset} (${data.size} bytes): ${data.message}`;
      });
      return;
    }
    if (data.t === 'done') { finishRun(data.code); return; }
  };
  worker.onerror = (error) => {
    addRow('fatal', (row) => { row.textContent = `worker failed: ${error.message || error}`; });
    finishRun(-1);
  };

  return worker;
}

let runStarted = 0;

async function run() {
  if (running || !selected) return;

  running = true;
  updateRunButton();
  clearTrace();

  // Restated per run, because clearing the trace takes the root's own attach messages with it -- and
  // what is attached is the first thing worth knowing when a dynamic binary cannot find a library.
  if (hosted.size || files.size) {
    addRow('sysinfo', (row) => {
      row.textContent =
        `Root: ${files.size} file${files.size === 1 ? '' : 's'} in memory, ` +
        `${hosted.size} served from ${location.origin}/macos-root.`;
    });
  }

  moduleCount = 0;
  window.__lastExit = null;
  window.__lastMemory = null;
  window.__lastCounters = null;
  kvTable(el.resultInfo, [['state', 'running']]);
  setStatus('running…', 'running');
  runStarted = performance.now();
  el.stop.disabled = false;
  el.screenStage.classList.add('live');

  const args = buildArguments();
  addRow('sysinfo', (row) => { row.textContent = `$ macos-web ${args.join(' ')}`; });

  try {
    const w = ensureWorker();

    // Sent as full host paths, because that is what reaches the range bridge: the emulator resolves a
    // guest path through the emulation root before it ever asks for bytes.
    const handles = [...files].map(([path, file]) => [GUEST_ROOT + path, file]);

    const attached = [...hosted].map(([path, entry]) => ({ path: GUEST_ROOT + path, url: entry.url }));

    w.postMessage({ t: 'files', files: handles });
    const dirs = [...hostedDirs].map(([path, url]) => ({ path: GUEST_ROOT + path, url }));

    w.postMessage({ t: 'run', args, memfs: [], root: GUEST_ROOT, hosted: attached, hostedDirs: dirs, served: SERVED_ROOT });
  } catch (error) {
    addRow('fatal', (row) => { row.textContent = `could not start the worker: ${error && error.message ? error.message : error}`; });
    finishRun(-1);
  }
}

// A run leaves the worker's module exited and unusable, so it is discarded and the next run gets a fresh
// one. main() executes exactly once per instance.
function finishRun(code) {
  if (!running) return;

  el.stop.disabled = true;
  el.screenStage.classList.remove('live');
  reportResult(code, performance.now() - runStarted);

  if (worker) { worker.terminate(); worker = null; }

  running = false;
  updateRunButton();
  flushRows();
}

function reportResult(code, elapsed) {
  const pairs = [];
  const exit = window.__lastExit;

  if (exit && typeof exit.status === 'number') {
    pairs.push(['guest exit', String(exit.status)]);
    setStatus(exit.status === 0 ? 'exited 0' : `exited ${exit.status}`, exit.status === 0 ? 'ok' : 'bad');
  } else if (exit && exit.stopped) {
    pairs.push(['stopped', exit.stopped]);
    if (exit.detail) pairs.push(['detail', exit.detail]);
    if (exit.pc) pairs.push(['pc', exit.pc]);
    setStatus(exit.stopped, 'bad');
  } else if (exit && exit.fatal) {
    pairs.push(['fatal', exit.fatal]);
    setStatus('failed', 'bad');
  } else {
    pairs.push(['module exit', String(code)]);
    setStatus('finished', code === 0 ? 'ok' : 'bad');
  }

  pairs.push(['wall clock', `${elapsed.toFixed(0)} ms`]);

  const counters = window.__lastCounters;
  if (counters && Number(counters.counted)) {
    pairs.push(['instructions', Number(counters.instructions).toLocaleString()]);
    pairs.push(['basic blocks', Number(counters.blocks).toLocaleString()]);
  } else {
    pairs.push(['instructions', 'not counted here']);
  }

  pairs.push(['trace lines', String(traceRows)]);

  const memory = window.__lastMemory;
  if (memory) {
    pairs.push(['committed', bytes(Number(memory.committed))]);
    pairs.push(['reserved', bytes(Number(memory.reserved))]);
    pairs.push(['regions', String(memory.regions)]);
    pairs.push(['wasm heap', bytes(Number(memory.host))]);
  }

  kvTable(el.resultInfo, pairs);
}

/* ------------------------------------------------------------------- wiring */

$('pick-files').addEventListener('click', () => $('file-input').click());
$('pick-dir').addEventListener('click', () => $('dir-input').click());
$('pick-bundle').addEventListener('click', () => $('bundle-input').click());
$('file-input').addEventListener('change', (e) => addFromInput(e.target.files));
$('dir-input').addEventListener('change', (e) => addFromInput(e.target.files));
$('bundle-input').addEventListener('change', (e) => addFromInput(e.target.files));

// Attached from the local server rather than picked. A folder picker cannot be used for a macOS root
// anyway: it is built out of symlinks and browsers do not follow them, so it looks like an empty
// directory. These read by range and download nothing.
async function headSize(url) {
  const response = await fetch(url, { method: 'HEAD' });
  if (!response.ok) throw new Error(`${response.status} for ${url}`);

  return Number(response.headers.get('content-length') || 0);
}

async function attachHosted(guestPath, url) {
  const size = await headSize(url);
  hosted.set(guestPath, { url, size });
  return size;
}

// The server's listing is the only index the page has of the root, and it is enough: an href ends in a
// slash exactly when the entry is a directory, a symlinked one included. Nothing here resolves a link --
// a macOS root is built out of them, and following one to its target is how a walk leaves the root.
async function listServed(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${response.status} listing ${url}`);

  const html = await response.text();
  return [...html.matchAll(/href="([^"?]+)"/g)].map((match) => {
    const directory = match[1].endsWith('/');
    const href = directory ? match[1].slice(0, -1) : match[1];
    return { href, name: decodeURIComponent(href), directory };
  });
}

// A link pointing back at one of its own ancestors is the single shape the trailing-slash rule cannot
// see through, so the descent is bounded rather than tracked. No bundle nests anywhere near this deep.
const SERVED_WALK_DEPTH = 12;

async function walkServed(url, prefix, found = [], depth = 0) {
  if (depth >= SERVED_WALK_DEPTH) return found;

  for (const entry of await listServed(`${url}/`)) {
    if (entry.directory) {
      await walkServed(`${url}/${entry.href}`, `${prefix}/${entry.name}`, found, depth + 1);
    } else {
      found.push([`${prefix}/${entry.name}`, { url: `${url}/${entry.href}` }]);
    }
  }

  return found;
}

async function attachMacosRoot() {
  try {
    // arm64e only. The same directory holds an x86_64 set that an arm64 guest never reads, and a
    // dozen extra rows makes it harder to see what is actually there.
    const names = (await listServed(`${SERVED_ROOT}/System/Library/dyld/`))
      .filter((entry) => !entry.directory && entry.name.startsWith('dyld_shared_cache_arm64e'))
      .map((entry) => entry.name);

    let total = 0;
    total += await attachHosted('/usr/lib/dyld', `${SERVED_ROOT}/usr/lib/dyld`);

    for (const name of names) {
      total += await attachHosted(`/System/Library/dyld/${name}`, `${SERVED_ROOT}/System/Library/dyld/${name}`);
    }

    // CoreFoundation reads it for the OS version. Optional: a root without it still runs anything that
    // does not reach CoreFoundation.
    try {
      total += await attachHosted('/System/Library/CoreServices/SystemVersion.plist',
                                  `${SERVED_ROOT}/System/Library/CoreServices/SystemVersion.plist`);
    } catch { /* absent is not fatal */ }

    addRow('sysinfo', (row) => {
      row.textContent =
        `Attached ${hosted.size} files from the served macOS root, ${bytes(total)} total. ` +
        `Nothing was downloaded -- ranges are read as the guest reaches them.`;
    });

    renderTree();
    updateRunButton();
    refreshCache();
  } catch (error) {
    addRow('fatal', (row) => {
      row.textContent =
        `Could not reach the served macOS root: ${error.message}. Start the server with ` +
        `--macos-root pointing at a directory holding usr/lib/dyld and System/Library/dyld.`;
    });
  }
}

$('pick-dyld').addEventListener('click', attachMacosRoot);

// A real app bundle, walked off the served root instead of picked. A folder picker cannot reach
// /System/Applications at all, and one that could would copy the whole bundle in before the guest asked
// for any of it. The bundle path is the only thing this knows about the app: the files go through the
// same absorb() a dropped .app takes, and Info.plist still decides which binary runs.
async function attachServedBundle(bundlePath) {
  const name = bundlePath.split('/').filter(Boolean).pop();

  try {
    if (![...hosted.keys()].some((path) => path.includes('dyld_shared_cache'))) await attachMacosRoot();

    const found = await walkServed(`${SERVED_ROOT}${bundlePath}`, name);
    if (!found.length) throw new Error(`${bundlePath} holds no files`);

    const entries = await Promise.all(
      found.map(async ([relative, source]) => [relative, { url: source.url, size: await headSize(source.url) }]));

    const mount = normalize($('opt-bundle-mount').value || '/Applications');
    hostedDirs.set(`${mount}/${name}`, `${SERVED_ROOT}${bundlePath}`);

    await absorb(entries);
  } catch (error) {
    addRow('fatal', (row) => {
      row.textContent =
        `Could not attach ${bundlePath} from the served macOS root: ${error.message}. Start the server ` +
        `with --macos-root pointing at a root that holds it.`;
    });
  }
}

// A macOS root arrives as a folder whose own name is not part of the guest path: what the emulator wants
// is /usr/lib/dyld and /System/Library/dyld, not /my-root/usr/lib/dyld. The first path component is
// dropped so the picked folder becomes the root itself.
$('pick-root').addEventListener('click', () => $('root-input').click());
$('root-input').addEventListener('change', async (e) => {
  for (const file of e.target.files) {
    const relative = file.webkitRelativePath || file.name;
    const stripped = relative.split('/').slice(1).join('/');
    if (stripped) await addFile(stripped, file);
  }
  renderTree();
});

$('wipe').addEventListener('click', () => {
  files.clear();
  headers.clear();
  hosted.clear();
  hostedDirs.clear();
  selected = null;
  el.selPath.textContent = '—';
  renderTree();
  updateRunButton();
});

// The demos live inside the wasm module as embedded files, so they are read out through a throwaway
// instance rather than fetched over the network.
async function addEmbeddedDemo(embeddedPath, guestPath, label) {
  if (!moduleFactory) return;
  try {
    const probe = await moduleFactory({ noInitialRun: true, print: () => {}, printErr: () => {} });
    const data = probe.FS.readFile(embeddedPath);
    await addFile(guestPath, new File([data], label));
    select(guestPath);
    renderTree();
  } catch (error) {
    addRow('fatal', (row) => { row.textContent = `could not read the demo binary: ${error}`; });
  }
}

$('add-demo').addEventListener('click', () => addEmbeddedDemo(DEMO_PATH, '/bin/trace-demo', 'trace-demo'));

// Dynamically linked against SkyLight, so it needs a macOS root before it will get past dyld. Ticking
// the window-server box is what makes its windows show up rather than only its trace.
$('add-gui-demo').addEventListener('click', async () => {
  await addEmbeddedDemo(GUI_DEMO_PATH, '/bin/calc-demo', 'calc-demo');
  $('opt-gui').checked = true;
  $('opt-desktop').value = '420x520';
});

$('add-paint-demo').addEventListener('click', async () => {
  await addEmbeddedDemo(PAINT_DEMO_PATH, '/bin/paint-demo', 'paint-demo');
  $('opt-gui').checked = true;
  $('opt-desktop').value = '420x520';
});

// A desktop the app's own window fits in. Calculator's is portrait and about 460x800, so the landscape
// default would clip it vertically; a bigger desktop than that only costs frame bytes.
$('load-calculator').addEventListener('click', async () => {
  await attachServedBundle(CALCULATOR_BUNDLE);
  $('opt-desktop').value = '640x900';
});

el.run.addEventListener('click', run);

// Terminating the worker is the only way to interrupt a run: the guest owns that thread for as long as
// it executes, so nothing inside it can be asked to stop.
el.stop.addEventListener('click', () => {
  if (!running) return;
  addRow('err', (row) => { row.textContent = 'Stopped by request.'; });
  finishRun(-1);
});
el.clear.addEventListener('click', () => { clearTrace(); kvTable(el.resultInfo, []); setStatus('ready'); });
el.filter.addEventListener('input', applyFilter);
for (const box of document.querySelectorAll('#kinds input')) box.addEventListener('change', applyFilter);

for (const event of ['dragenter', 'dragover']) {
  el.drop.addEventListener(event, (e) => { e.preventDefault(); el.drop.classList.add('over'); });
}
for (const event of ['dragleave', 'drop']) {
  el.drop.addEventListener(event, () => el.drop.classList.remove('over'));
}
el.drop.addEventListener('drop', async (e) => {
  e.preventDefault();
  if (e.dataTransfer.items && e.dataTransfer.items.length) {
    await addFromDataTransfer(e.dataTransfer.items);
  } else {
    await addFromInput(e.dataTransfer.files);
  }
});
document.addEventListener('dragover', (e) => e.preventDefault());
document.addEventListener('drop', (e) => e.preventDefault());

/* ------------------------------------------------------------------ startup */

(async function boot() {
  renderTree();
  clearTrace();
  installInputBridge();

  try {
    // Loaded here only to read the embedded demo binary out of it. The run itself happens in the worker,
    // which needs FileReaderSync -- it does not exist on the main thread, and the range bridge cannot
    // await anything because it is called from inside a memory fault.
    const loaded = await import('./macos-web.js');
    moduleFactory = loaded.default;
    setStatus('ready', 'ok');
  } catch (error) {
    setStatus('module failed to load', 'bad');
    addRow('fatal', (row) => { row.textContent = `could not load macos-web.js: ${error}`; });
  }

  updateRunButton();
})();

/* ------------------------------------------------------------------- cache */

// A visit re-read the whole emulation root over the network every time: the guest pages the shared
// cache in by byte range from inside a memory fault, and the browser's own HTTP cache will not hold
// sparse entries for a multi-gigabyte file -- measured, 14 of 16 repeated 2 MiB reads still went to the
// network even once the server sent proper validators. A service worker answers those ranges out of
// Cache Storage instead, where the quota is hundreds of gigabytes. The read path itself is untouched.

let cacheWorker = null;
let cacheWarming = false;
let cacheCancelled = false;
let cacheReplyId = 0;
const cacheReplies = new Map();

function cacheAsk(message) {
  const worker = navigator.serviceWorker.controller;
  if (!worker) return Promise.reject(new Error('no service worker is controlling this page'));

  const id = ++cacheReplyId;
  return new Promise((resolve, reject) => {
    cacheReplies.set(id, { resolve, reject });
    worker.postMessage({ ...message, id });
    setTimeout(() => {
      if (cacheReplies.delete(id)) reject(new Error('the service worker did not answer'));
    }, 600000);
  });
}

// Off is a real answer, not a debug escape hatch: over a network the cache is the difference between a
// usable playground and an unusable one, but against a server on this machine the root is already
// nearby and caching it again only adds a copy. The query parameter is what lets a harness say so
// without a click.
function rootCacheWanted() {
  const asked = new URLSearchParams(location.search).get('rootcache');
  if (asked === 'off' || asked === '0') return false;
  if (asked === 'on' || asked === '1') return true;
  return localStorage.getItem('sogen-root-cache') !== 'off';
}

async function registerCacheWorker() {
  if (!('serviceWorker' in navigator)) {
    renderCache({ unsupported: true });
    return;
  }

  const wanted = rootCacheWanted();
  $('opt-root-cache').checked = wanted;

  if (!wanted) {
    for (const registration of await navigator.serviceWorker.getRegistrations()) await registration.unregister();
    renderCache({ disabled: true });
    return;
  }

  navigator.serviceWorker.addEventListener('message', (event) => {
    const pending = cacheReplies.get(event.data && event.data.id);
    if (!pending) return;
    cacheReplies.delete(event.data.id);
    pending.resolve(event.data);
  });

  try {
    cacheWorker = await navigator.serviceWorker.register('./sw.js', { scope: './' });
  } catch (error) {
    renderCache({ error: String(error.message || error) });
    return;
  }

  // Asking for persistence matters more than the quota does: without it the origin is best-effort and
  // a warmed root can be evicted under disk pressure, which surfaces as a run that is mysteriously
  // slow again rather than as anything a user could diagnose.
  try {
    if (navigator.storage && navigator.storage.persist) await navigator.storage.persist();
  } catch { /* a refusal is not a failure; the cache still works */ }

  // A first visit is not controlled until the worker activates, and an uncontrolled page reads straight
  // past the cache. Waiting for it here is what makes the very first run benefit too.
  if (!navigator.serviceWorker.controller) {
    await new Promise((resolve) => {
      const done = () => resolve();
      navigator.serviceWorker.addEventListener('controllerchange', done, { once: true });
      setTimeout(done, 3000);
    });
  }

  await refreshCache();
}

function rootTargets() {
  const targets = new Map();
  for (const entry of hosted.values()) {
    if (!entry.url || !entry.url.includes('/macos-root/')) continue;
    targets.set(entry.url, Number(entry.size) || 0);
  }
  return targets;
}

async function refreshCache() {
  if (!navigator.serviceWorker.controller) {
    renderCache({ uncontrolled: true });
    return;
  }

  let status = null;
  try {
    status = await cacheAsk({ t: 'sogen-cache-status' });
  } catch (error) {
    renderCache({ error: String(error.message || error) });
    return;
  }

  let estimate = null;
  try {
    estimate = navigator.storage && navigator.storage.estimate ? await navigator.storage.estimate() : null;
  } catch { /* not everywhere */ }

  const persisted = navigator.storage && navigator.storage.persisted ? await navigator.storage.persisted() : false;

  // What a completed Prepare left behind, so a cache that has since shrunk can be named as evicted
  // rather than quietly behaving like a cold one.
  const prepared = Number(localStorage.getItem('sogen-root-cache-blocks')) || 0;

  renderCache({ status, estimate, persisted, prepared, targets: rootTargets() });
}

function renderCache(state) {
  const dot = $('cache-dot');
  const text = $('cache-state');
  const detail = $('cache-detail');

  $('cache-warm').disabled = cacheWarming || !navigator.serviceWorker.controller;
  $('cache-clear').disabled = cacheWarming || !navigator.serviceWorker.controller;

  if (state.disabled) {
    dot.className = 'cache-dot';
    text.textContent = 'local root cache: off — the root is read over the network on every run';
    detail.textContent = '';
    $('cache-warm').disabled = true;
    $('cache-clear').disabled = true;
    return;
  }

  if (state.unsupported) {
    dot.className = 'cache-dot off';
    text.textContent = 'local root cache: unavailable (no service worker in this browser)';
    detail.textContent = 'the root is re-read over the network on every visit';
    return;
  }

  if (state.error) {
    dot.className = 'cache-dot off';
    text.textContent = `local root cache: ${state.error}`;
    detail.textContent = '';
    return;
  }

  if (state.uncontrolled) {
    dot.className = 'cache-dot off';
    text.textContent = 'local root cache: installing — reload to enable it';
    detail.textContent = '';
    return;
  }

  const cached = state.status.bytes || 0;
  const blocks = state.status.blocks || 0;
  const wanted = [...(state.targets || new Map()).values()].reduce((a, b) => a + b, 0);
  const evicted = Math.max(0, (state.prepared || 0) - blocks);

  const warnings = [];
  if (!state.persisted) {
    warnings.push('NOT PERSISTENT — the browser may delete this cache at any time, including part-way ' +
      'through a run. Add the page to your bookmarks or grant storage permission to keep it.');
  }
  if (evicted > 0) {
    warnings.push(`${evicted} of ${state.prepared} prepared blocks (${bytes(evicted * (state.status.block || 2097152))}) ` +
      'have been evicted — press Prepare root again.');
  }
  if (state.estimate && state.estimate.quota && wanted) {
    const room = state.estimate.quota - (state.estimate.usage || 0) + cached;
    if (wanted > room) {
      warnings.push(`the attached root is ${bytes(wanted)} and only ${bytes(room)} of quota is available, ` +
        'so it cannot be held whole.');
    }
  }

  dot.className = 'cache-dot ' +
    (warnings.length ? (cached === 0 ? 'off' : 'partial')
                     : (cached === 0 ? 'off' : (wanted && cached >= wanted * 0.98 ? 'ready' : 'partial')));

  text.textContent = cached === 0
    ? 'local root cache: empty — the root will be fetched over the network'
    : `local root cache: ${bytes(cached)} in ${blocks} blocks` + (wanted ? ` of ${bytes(wanted)} attached` : '');

  const parts = [];
  if (state.estimate && state.estimate.quota) {
    parts.push(`${bytes(state.estimate.usage || 0)} used of ${bytes(state.estimate.quota)}`);
  }
  parts.push(state.persisted ? 'storage is persistent' : 'storage is best-effort');
  detail.textContent = parts.join(' · ');

  const alert = $('cache-warning');
  alert.hidden = warnings.length === 0;
  alert.textContent = warnings.join(' ');
}

function setCacheProgress(fraction, label) {
  const track = $('cache-track');
  track.hidden = fraction === null;
  if (fraction !== null) $('cache-fill').style.width = `${Math.round(fraction * 100)}%`;
  if (label !== undefined) $('cache-detail').textContent = label;
}

// Warmed in windows rather than whole files, so Cancel takes effect in a second or two rather than at
// the end of a 1.7 GB subcache.
const CACHE_WARM_WINDOW = 64;

async function warmCache() {
  const targets = rootTargets();

  if (!targets.size) {
    setCacheProgress(null, 'attach a macOS root first — there is nothing to prepare');
    return;
  }

  cacheWarming = true;
  cacheCancelled = false;
  $('cache-cancel').hidden = false;
  renderCache({ status: { bytes: 0, blocks: 0 }, targets });

  const total = [...targets.values()].reduce((a, b) => a + b, 0);
  const started = performance.now();
  let done = 0;

  try {
    for (const [url, size] of targets) {
      if (cacheCancelled) break;

      const status = await cacheAsk({ t: 'sogen-cache-status' });
      const blocks = Math.max(1, Math.ceil(size / (status.block || 2097152)));

      for (let from = 0; from < blocks; from += CACHE_WARM_WINDOW) {
        if (cacheCancelled) break;

        const to = Math.min(from + CACHE_WARM_WINDOW - 1, blocks - 1);
        const answer = await cacheAsk({ t: 'sogen-cache-warm', url, from, to });
        if (answer.error) throw new Error(`${url.split('/').pop()}: ${answer.error}`);

        done += Math.min((to - from + 1) * (status.block || 2097152), Math.max(0, size - from * (status.block || 2097152)));
        const rate = done / ((performance.now() - started) / 1000);
        setCacheProgress(total ? done / total : 0,
          `${bytes(done)} of ${bytes(total)} · ${bytes(rate)}/s · ${url.split('/').pop()}`);
      }
    }
  } catch (error) {
    addRow('fatal', (row) => { row.textContent = `could not prepare the root cache: ${error.message || error}`; });
  } finally {
    cacheWarming = false;
    setCacheProgress(null);

    // Only a Prepare that ran to the end sets the mark, or a cancelled one would make everything it
    // did not reach look like eviction ever after.
    if (!cacheCancelled) {
      try {
        const status = await cacheAsk({ t: 'sogen-cache-status' });
        localStorage.setItem('sogen-root-cache-blocks', String(status.blocks || 0));
      } catch { /* the mark is a convenience, not a correctness thing */ }
    }

    await refreshCache();
    $('cache-cancel').hidden = true;
  }
}

$('cache-warm').addEventListener('click', warmCache);
$('cache-cancel').addEventListener('click', () => { cacheCancelled = true; });
$('cache-clear').addEventListener('click', async () => {
  try {
    localStorage.removeItem('sogen-root-cache-blocks');
    await cacheAsk({ t: 'sogen-cache-clear' });
  } catch (error) {
    addRow('fatal', (row) => { row.textContent = `could not clear the root cache: ${error.message || error}`; });
  }
  await refreshCache();
});

$('opt-root-cache').addEventListener('change', async (event) => {
  localStorage.setItem('sogen-root-cache', event.target.checked ? 'on' : 'off');
  location.reload();
});

registerCacheWorker();
