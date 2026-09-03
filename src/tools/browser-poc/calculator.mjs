// Drives the page's one-click bundle attach and then runs what it selected. Nothing here names a file
// inside the app: the button walks the served root, Info.plist picks the binary, and this only checks
// that the whole subtree arrived served rather than copied and reports where the guest got to.

import { launchBrowser } from './browser.mjs';

const URL = process.env.POC_URL || 'http://127.0.0.1:8110/';
const RUN_TIMEOUT_MS = Number(process.env.POC_RUN_TIMEOUT_MS || 60 * 60 * 1000);
const ATTACH_ONLY = process.env.POC_ATTACH_ONLY === '1';
const SHOT = '/tmp/browser-calculator.png';

const consoleErrors = [];
let stage = 'launch';

function fail(message) {
  throw new Error(message);
}

const browser = await launchBrowser('calculator.mjs');
if (!browser) process.exit(0);

let code = 0;
let progress = null;

try {
  const page = await browser.newPage({ viewport: { width: 1500, height: 1000 } });
  page.on('console', (msg) => { if (msg.type() === 'error') consoleErrors.push(msg.text()); });
  page.on('pageerror', (error) => consoleErrors.push(String(error)));

  stage = 'open page';
  await page.goto(URL, { waitUntil: 'load' });
  await page.waitForFunction(() => document.getElementById('status').textContent === 'ready', null, { timeout: 120000 });
  console.log('page ready');

  stage = 'load calculator';
  const attachStart = Date.now();
  await page.click('#load-calculator');
  await page.waitForFunction(() => document.getElementById('sel-path').textContent.includes('/Contents/MacOS/'), null,
    { timeout: 120000 });
  console.log(`attach took ${((Date.now() - attachStart) / 1000).toFixed(1)} s`);

  const attached = await page.evaluate(() => ({
    selected: document.getElementById('sel-path').textContent,
    count: document.getElementById('fs-count').textContent,
    gui: document.getElementById('opt-gui').checked,
    rows: [...document.querySelectorAll('#trace > *')].map((r) => r.textContent)
      .filter((t) => t.startsWith('Attached ') || t.startsWith('Launching ') || t.startsWith('Could not')),
    served: [...document.querySelectorAll('#tree .node.file')]
      .filter((n) => n.textContent.includes('served')).length,
    copied: [...document.querySelectorAll('#tree .node.file')]
      .filter((n) => !n.textContent.includes('served')).length,
    paths: [...document.querySelectorAll('#tree .node.file')].map((n) => n.title.split(' ')[0]),
  }));

  for (const row of attached.rows) console.log(`  ${row}`);
  console.log(`selected: ${attached.selected}`);
  console.log(`filesystem: ${attached.count} (${attached.served} served, ${attached.copied} copied), gui: ${attached.gui}`);

  const mount = await page.inputValue('#opt-bundle-mount');
  const inside = attached.paths.filter((p) => p.startsWith(`${mount}/Calculator.app/`));
  console.log(`${inside.length} file(s) under ${mount}/Calculator.app, ${attached.paths.length - inside.length} elsewhere`);

  if (!attached.selected.endsWith('/Contents/MacOS/Calculator')) fail(`selected ${attached.selected}`);
  if (attached.copied !== 0) fail(`${attached.copied} file(s) were copied into memory instead of served`);
  if (!attached.rows.some((t) => t.includes('CFBundleExecutable'))) fail('the binary was not chosen from Info.plist');

  // The bundle's own count, not a round number: the walk has to reach every leaf of the subtree, and the
  // nested widget and the asset catalogue are the two it would quietly miss.
  for (const rest of ['Contents/Resources/Assets.car', 'Contents/PlugIns/CalculatorWidget.appex/Contents/MacOS/CalculatorWidget']) {
    if (!inside.includes(`${mount}/Calculator.app/${rest}`)) fail(`the walk missed ${rest}`);
  }
  if (inside.length < 20) fail(`only ${inside.length} file(s) under the mount point`);

  await page.screenshot({ path: SHOT, fullPage: true });
  console.log(`screenshot saved to ${SHOT}`);

  if (ATTACH_ONLY) {
    console.log(`PASS (attach only): ${attached.count} attached, ${attached.selected} selected`);
  } else {
    stage = 'run';
    await page.click('#run');
    const runStart = Date.now();
    console.log('run started');

    progress = setInterval(async () => {
      try {
        const s = await page.evaluate(() => ({
          status: document.getElementById('status').textContent,
          rows: document.getElementById('trace-count').textContent,
          syscalls: document.getElementById('sys-total').textContent,
          modules: document.getElementById('mod-count').textContent,
          last: [...document.querySelectorAll('#trace > .sys')].slice(-1).map((r) => r.textContent)[0] || '',
          stream: window.__stream ? { drawn: window.__stream.drawn, presents: window.__stream.presents,
                                      threads: window.__stream.threads, windows: window.__stream.guestWindows } : null,
          exit: window.__lastExit,
        }));
        console.log(`  [${Math.round((Date.now() - runStart) / 1000)}s] ${s.status} rows=${s.rows} syscalls=${s.syscalls} ` +
          `modules=${s.modules} stream=${JSON.stringify(s.stream)} last=${s.last.slice(0, 80)} exit=${JSON.stringify(s.exit)}`);
      } catch { /* ignore */ }
    }, 30000);

    await page.waitForFunction(() => window.__lastExit != null, null, { timeout: RUN_TIMEOUT_MS });
    clearInterval(progress);

    const done = await page.evaluate(() => ({
      exit: window.__lastExit,
      counters: window.__lastCounters,
      status: document.getElementById('status').textContent,
      modules: document.getElementById('mod-count').textContent,
      syscalls: document.getElementById('sys-total').textContent,
      stream: window.__stream ? { ...window.__stream } : null,
      failures: [...document.querySelectorAll('#failures > div')].map((r) => r.textContent),
      tail: [...document.querySelectorAll('#trace > *')].slice(-40).map((r) => r.textContent),
      enoent: [...document.querySelectorAll('#trace > .det')].map((r) => r.textContent)
        .filter((t) => t.startsWith('path: ')).slice(-40),
    }));

    console.log(`wall clock: ${((Date.now() - runStart) / 1000).toFixed(0)} s`);
    console.log(`exit: ${JSON.stringify(done.exit)}`);
    console.log(`counters: ${JSON.stringify(done.counters)}`);
    console.log(`modules: ${done.modules}, syscalls: ${done.syscalls}, stream: ${JSON.stringify(done.stream)}`);
    console.log('failure counts:');
    for (const line of done.failures) console.log(`  ${line}`);
    console.log('trace tail:');
    for (const line of done.tail) console.log(`  ${line}`);

    await page.screenshot({ path: SHOT, fullPage: true });
    console.log(`screenshot saved to ${SHOT}`);
    console.log('DONE');
  }

  if (consoleErrors.length) {
    console.log('console errors:');
    for (const line of consoleErrors) console.log(`  ${line}`);
  } else {
    console.log('no console errors');
  }
} catch (error) {
  code = 1;
  clearInterval(progress);
  console.error(`FAIL [${stage}]: ${error && error.message ? error.message.split('\n')[0] : error}`);
  try {
    const page = browser.contexts()[0].pages()[0];
    const dump = await page.evaluate(() => ({
      status: document.getElementById('status').textContent,
      selected: document.getElementById('sel-path').textContent,
      exit: window.__lastExit,
      tail: [...document.querySelectorAll('#trace > *')].slice(-40).map((r) => r.textContent),
    }));
    console.error(`status=${dump.status} selected=${dump.selected} exit=${JSON.stringify(dump.exit)}`);
    for (const line of dump.tail) console.error(`  ${line}`);
    await page.screenshot({ path: SHOT, fullPage: true });
    console.error(`screenshot saved to ${SHOT}`);
  } catch { /* page may be gone */ }
  for (const line of consoleErrors) console.error(`  console: ${line}`);
} finally {
  await browser.close().catch(() => {});
}

process.exit(code);
