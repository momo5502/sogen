// The page/ counterpart of calculator.mjs: attaches a real .app through page/'s own macOS attach panel
// -- once from disk through the directory input, once off the served root -- and then runs what
// Info.plist selected. Nothing here names a file inside the app.
//
// Point it at a server that has both page/dist and a macOS root:
//   python3 src/macos-web/serve.py --port 8120 --directory page/dist --macos-root /path/to/root
//
// macOS mode is written straight into localStorage rather than clicked in the settings popover, so the
// driver does not depend on that menu's markup; the radio itself is checked by modes.mjs.

import { launchBrowser } from './browser.mjs';

const URL = process.env.POC_URL || 'http://127.0.0.1:8120/';
const BUNDLE = process.env.POC_BUNDLE || '/System/Applications/Calculator.app';
const WATCH_MS = Number(process.env.POC_WATCH_MS || 15 * 60 * 1000);
const SAMPLE_MS = Number(process.env.POC_SAMPLE_MS || 30000);
const ATTACH_ONLY = process.env.POC_ATTACH_ONLY === '1';
const SHOT = process.env.POC_SHOT || '/tmp/page-calculator.png';

const SETTINGS = {
  logging: 'regular', bufferStdout: true, persist: false, execAccess: false, foreignAccess: false,
  wasm64: false, instructionSummary: false, ignoredFunctions: [], interestingModules: [],
  environmentVariables: [], commandLine: '', mode: 'macos',
};

const consoleErrors = [];
let stage = 'launch';

function fail(message) {
  throw new Error(message);
}

const browser = await launchBrowser('page-calculator.mjs');
if (!browser) process.exit(0);

let code = 0;

try {
  const page = await browser.newPage({ viewport: { width: 1600, height: 1100 } });
  page.on('console', (msg) => { if (msg.type() === 'error') consoleErrors.push(msg.text()); });
  page.on('pageerror', (error) => consoleErrors.push(String(error)));

  // An init script is evaluated inside a function scope, so a bare declaration never reaches window --
  // everything the driver reads back is assigned onto it explicitly. The Worker subclass is how a run is
  // observed at all: page/ renders its trace into a virtualised list, so the DOM holds only the rows
  // that happen to be on screen.
  await page.addInitScript((raw) => {
    localStorage.setItem('settings', raw);
    window.__log = [];
    window.__events = [];
    window.__frames = 0;
    window.__firstFrameAt = null;
    window.__end = null;

    const Base = window.Worker;
    window.Worker = class extends Base {
      constructor(url, options) {
        super(url, options);
        this.addEventListener('message', (event) => {
          const data = event.data;
          if (!data) return;
          if (data.message === 'log' && Array.isArray(data.data)) {
            for (const line of data.data) {
              window.__log.push(line);
              if (window.__log.length > 80000) window.__log.shift();
            }
          }
          if (data.message === 'macos-status') window.__events.push(data.event);
          if (data.message === 'end') window.__end = data.data;
          if (data.t === 'frame') {
            if (!window.__frames) window.__firstFrameAt = performance.now();
            ++window.__frames;
          }
        });
      }
    };
  }, JSON.stringify(SETTINGS));

  const attachedState = () => page.evaluate(() => ({
    note: document.querySelector('[data-testid="macos-attach-note"]')?.textContent || null,
    summary: document.querySelector('[data-testid="macos-attached"]')?.textContent || null,
  }));

  const waitForLaunchable = () => page.waitForFunction(() => {
    const note = document.querySelector('[data-testid="macos-attach-note"]');
    return !!note && note.textContent.includes('Launching ');
  }, null, { timeout: 180000 });

  stage = 'open playground';
  await page.goto(`${URL}#/playground`, { waitUntil: 'load' });
  // A HashRouter turns a second goto to the same hash into a same-document navigation, which runs
  // neither the init script nor loadSettings again.
  await page.reload({ waitUntil: 'load' });
  await page.waitForSelector('[data-testid="macos-bundle-input"]', { state: 'attached', timeout: 60000 });
  console.log('macOS mode reached, attach panel present');

  stage = 'attach from disk';
  const diskStart = Date.now();
  await page.setInputFiles('[data-testid="macos-bundle-input"]', BUNDLE);
  await waitForLaunchable();
  const disk = await attachedState();
  console.log(`from disk (${((Date.now() - diskStart) / 1000).toFixed(2)} s): ${disk.summary}`);
  console.log(`  ${disk.note}`);
  if (!disk.note.includes('/Contents/MacOS/')) fail(`no executable selected: ${disk.note}`);

  stage = 'clear';
  await page.click('button:has-text("Clear")');
  await page.waitForFunction(() => !document.querySelector('[data-testid="macos-attached"]'), null,
    { timeout: 10000 });

  stage = 'attach from the served root';
  const servedStart = Date.now();
  await page.fill('input[aria-label="Bundle path on the served macOS root"]', BUNDLE);
  await page.click('button:has-text("Attach from served root")');
  await waitForLaunchable();
  const served = await attachedState();
  console.log(`from the served root (${((Date.now() - servedStart) / 1000).toFixed(2)} s): ${served.summary}`);
  console.log(`  ${served.note}`);
  if (!served.note.includes('/Contents/MacOS/')) fail(`no executable selected: ${served.note}`);
  if (!served.note.includes('CFBundleExecutable')) {
    console.log('note: the executable was not chosen from Info.plist; see the line above for the fallback');
  }

  await page.screenshot({ path: SHOT, fullPage: true });

  if (ATTACH_ONLY) {
    console.log(`PASS (attach only): ${served.summary}`);
  } else {
    stage = 'run';
    const runStart = Date.now();
    await page.click('[data-testid="macos-run-attached"]');
    console.log('run started');

    // A GUI guest does not exit -- the standalone page's own Calculator run reaches a live AppKit and
    // stays there -- so this samples for a fixed window and reports where it got to rather than waiting
    // for an exit that never comes.
    const deadline = Date.now() + WATCH_MS;
    let announcedFirstFrame = false;

    for (;;) {
      const sample = await page.evaluate(() => {
        const modules = new Set();
        let syscalls = 0, windows = 0, threads = 0, presents = 0, stopped = null, dyld = null;
        for (const event of window.__events) {
          if (event.t === 'module') modules.add(event.name || event.path || '');
          if (event.t === 'status') {
            syscalls = event.syscalls; windows = event.windows;
            threads = event.threads; presents = event.presents;
          }
          if (event.t === 'stopped') stopped = event.reason;
          if (event.t === 'exited') stopped = `exited ${event.status}`;
          if (event.t === 'dyld-error') dyld = event.message;
        }
        return {
          frames: window.__frames, firstFrameAt: window.__firstFrameAt, end: window.__end,
          lines: window.__log.length, modules: modules.size, syscalls, windows, threads, presents,
          stopped, dyld, last: window.__log.slice(-1)[0] || '',
        };
      });

      const seconds = Math.round((Date.now() - runStart) / 1000);
      if (sample.frames > 0 && !announcedFirstFrame) {
        announcedFirstFrame = true;
        console.log(`first frame at ~${seconds}s`);
      }
      console.log(`  [${seconds}s] frames=${sample.frames} lines=${sample.lines} modules=${sample.modules} ` +
        `syscalls=${sample.syscalls} windows=${sample.windows} threads=${sample.threads} ` +
        `presents=${sample.presents} stopped=${sample.stopped} dyld=${sample.dyld} ` +
        `last=${String(sample.last).slice(0, 90)}`);

      if (sample.end !== null || sample.stopped || sample.dyld) break;
      if (Date.now() >= deadline) break;
      await page.waitForTimeout(Math.min(SAMPLE_MS, Math.max(0, deadline - Date.now())));
    }

    const done = await page.evaluate(() => ({
      frames: window.__frames, end: window.__end, lines: window.__log.length,
      tail: window.__log.slice(-25),
      problems: window.__log
        .filter((l) => l.includes('could not attach') || l.includes('range read failed'))
        .slice(-10),
    }));

    console.log(`watched ${((Date.now() - runStart) / 1000).toFixed(0)} s, frames=${done.frames}, end=${done.end}`);
    if (done.problems.length) {
      console.log('attach problems:');
      for (const line of done.problems) console.log(`  ${line}`);
    }
    console.log('log tail:');
    for (const line of done.tail) console.log(`  ${String(line).slice(0, 150)}`);

    await page.screenshot({ path: SHOT, fullPage: true });
    console.log(`screenshot saved to ${SHOT}`);
    if (!done.frames) fail('the guest never produced a frame');
    console.log('PASS');
  }

  if (consoleErrors.length) {
    console.log('console errors:');
    for (const line of consoleErrors) console.log(`  ${line}`);
  } else {
    console.log('no console errors');
  }
} catch (error) {
  code = 1;
  console.error(`FAIL [${stage}]: ${error && error.message ? error.message.split('\n')[0] : error}`);
  try {
    const page = browser.contexts()[0].pages()[0];
    const dump = await page.evaluate(() => ({
      note: document.querySelector('[data-testid="macos-attach-note"]')?.textContent || null,
      warnings: [...document.querySelectorAll('.text-amber-600')].map((n) => n.textContent),
      frames: window.__frames, end: window.__end,
      tail: (window.__log || []).slice(-25),
    }));
    console.error(`note=${dump.note} frames=${dump.frames} end=${dump.end}`);
    for (const line of dump.warnings) console.error(`  warning: ${line}`);
    for (const line of dump.tail) console.error(`  ${String(line).slice(0, 150)}`);
    await page.screenshot({ path: SHOT, fullPage: true });
  } catch { /* page may be gone */ }
  for (const line of consoleErrors) console.error(`  console: ${line}`);
} finally {
  await browser.close().catch(() => {});
}

process.exit(code);
