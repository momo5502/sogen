import { launchBrowser } from './browser.mjs';

const URL = process.env.POC_URL || 'http://127.0.0.1:8099/';
const RUN_TIMEOUT_MS = 30 * 60 * 1000;
const SCREENSHOT = '/tmp/browser-poc.png';

const consoleErrors = [];
let stage = 'launch';

class StageFailure extends Error {}

function fail(message) {
  throw new StageFailure(message);
}

const browser = await launchBrowser('run.mjs');
if (!browser) process.exit(0);

let code = 0;
let progress = null;
try {
  const page = await browser.newPage({ viewport: { width: 1400, height: 1000 } });
  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  page.on('pageerror', (error) => consoleErrors.push(String(error)));

  stage = 'open page';
  await page.goto(URL, { waitUntil: 'load' });
  await page.waitForFunction(() => document.getElementById('status').textContent === 'ready', null, {
    timeout: 120000,
  });
  console.log('page ready');

  stage = 'attach macOS root';
  await page.click('#pick-dyld');
  await page.waitForFunction(
    () => document.getElementById('trace').textContent.includes('Attached') &&
          document.getElementById('trace').textContent.includes('macOS root'),
    null,
    { timeout: 60000 },
  );
  const attachLine = await page.evaluate(() =>
    [...document.querySelectorAll('#trace *')].map((r) => r.textContent).find((t) => t.includes('Attached')));
  console.log(`attached: ${attachLine}`);

  stage = 'add paint demo';
  await page.click('#add-paint-demo');
  await page.waitForFunction(() => document.getElementById('sel-path').textContent === '/bin/paint-demo', null, {
    timeout: 60000,
  });
  console.log('selected: /bin/paint-demo');

  stage = 'run';
  await page.click('#run');
  console.log('run started; waiting for the guest (first run pages the shared cache over ranges)...');

  const runStart = Date.now();
  const runDeadline = runStart + RUN_TIMEOUT_MS;
  progress = setInterval(async () => {
    try {
      const snapshot = await page.evaluate(() => ({
        status: document.getElementById('status').textContent,
        rows: document.getElementById('trace-count').textContent,
        syscalls: document.getElementById('sys-total').textContent,
        modules: document.getElementById('mod-count').textContent,
        exit: window.__lastExit || null,
      }));
      console.log(
        `progress [${Math.round((Date.now() - runStart) / 1000)}s]: status=${snapshot.status} ` +
          `trace-rows=${snapshot.rows} syscalls=${snapshot.syscalls} modules=${snapshot.modules} ` +
          `exit=${JSON.stringify(snapshot.exit)}`,
      );
    } catch { /* page navigating away is not expected, ignore */ }
  }, 30000);

  stage = 'window-server binding';
  await page.waitForFunction(
    () => document.getElementById('trace').textContent.includes('Window server: 21 of 21 routines intercepted'),
    null,
    { timeout: Math.max(1, runDeadline - Date.now()) },
  );
  console.log('window server: 21 of 21 routines intercepted');

  stage = 'screen painted';
  await page.waitForFunction(
    () => {
      const panel = document.getElementById('screen-panel');
      const canvas = document.getElementById('screen-canvas');
      return panel && !panel.hidden && canvas && canvas.width > 1 && window.__stream && window.__stream.drawn > 0;
    },
    null,
    { timeout: Math.max(1, runDeadline - Date.now()) },
  );
  console.log('screen panel visible with a streamed frame');

  stage = 'guest stdout + exit 0';
  await page.waitForFunction(
    () => document.getElementById('trace').textContent.includes('painted'),
    null,
    { timeout: Math.max(1, runDeadline - Date.now()) },
  );
  await page.waitForFunction(
    () => window.__lastExit && typeof window.__lastExit.status === 'number',
    null,
    { timeout: Math.max(1, runDeadline - Date.now()) },
  );
  const exitStatus = await page.evaluate(() => window.__lastExit.status);
  console.log(`guest stdout contained "painted"; exit status ${exitStatus}`);
  if (exitStatus !== 0) fail(`guest exited ${exitStatus}, expected 0`);

  stage = 'non-black pixels';
  const pixelStats = await page.evaluate(async () => {
    const canvas = document.getElementById('screen-canvas');
    const ctx = canvas.getContext('2d');
    const data = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
    let nonBlack = 0;
    for (let i = 0; i < data.length; i += 4) {
      if (data[i] > 8 || data[i + 1] > 8 || data[i + 2] > 8) ++nonBlack;
    }
    return { width: canvas.width, height: canvas.height, total: canvas.width * canvas.height, nonBlack };
  });
  console.log(
    `screen ${pixelStats.width}x${pixelStats.height}: ${pixelStats.nonBlack}/${pixelStats.total} non-black pixels`,
  );
  if (pixelStats.nonBlack < 10000) fail(`only ${pixelStats.nonBlack} non-black pixels, expected tens of thousands`);

  await page.screenshot({ path: SCREENSHOT, fullPage: true });
  clearInterval(progress);
  console.log(`screenshot saved to ${SCREENSHOT}`);

  if (consoleErrors.length) {
    console.log('console errors during the run:');
    for (const line of consoleErrors) console.log(`  ${line}`);
  } else {
    console.log('no console errors');
  }

  console.log('PASS: window server bound 21/21, screen painted, guest printed "painted" and exited 0');
} catch (error) {
  code = 1;
  clearInterval(progress);
  console.error(`FAIL [${stage}]: ${error && error.message ? error.message.split('\n')[0] : error}`);
  try {
    const dump = await browser.pages()[0].evaluate(() => ({
      status: document.getElementById('status').textContent,
      exit: window.__lastExit || null,
      traceTail: [...document.querySelectorAll('#trace > *')].slice(-25).map((r) => r.textContent),
    }));
    console.error(`page status: ${dump.status}`);
    console.error(`lastExit: ${JSON.stringify(dump.exit)}`);
    console.error('trace tail:');
    for (const line of dump.traceTail) console.error(`  ${line}`);
    await browser.pages()[0].screenshot({ path: SCREENSHOT, fullPage: true });
    console.error(`failure screenshot saved to ${SCREENSHOT}`);
  } catch { /* page may be gone */ }
  if (consoleErrors.length) {
    console.error('console errors captured:');
    for (const line of consoleErrors) console.error(`  ${line}`);
  }
} finally {
  await browser.close().catch(() => {});
}
process.exit(code);
