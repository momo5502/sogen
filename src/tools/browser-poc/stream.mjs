// Drives the page far enough to prove the three browser deltas: frames arrive while the guest is still
// running, a real click on the canvas reaches the emulator, and the page's counters move. Everything is
// checked against the page's own state rather than against the harness's expectations.

import { launchBrowser } from './browser.mjs';

const URL = process.env.POC_URL || 'http://127.0.0.1:8110/';
const RUN_TIMEOUT_MS = 40 * 60 * 1000;
const HOLD_MS = 120000;
const SHOT = '/tmp/browser-stream.png';

const consoleErrors = [];
let stage = 'launch';

function fail(message) {
  throw new Error(message);
}

const browser = await launchBrowser('stream.mjs');
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

  stage = 'attach macOS root';
  await page.click('#pick-dyld');
  await page.waitForFunction(() => document.getElementById('trace').textContent.includes('Attached'), null, { timeout: 60000 });
  console.log('macOS root attached');

  stage = 'add paint demo';
  await page.click('#add-paint-demo');
  await page.waitForFunction(() => document.getElementById('sel-path').textContent === '/bin/paint-demo', null, { timeout: 60000 });

  stage = 'run';
  await page.click('#run');
  const runStart = Date.now();
  const deadline = runStart + RUN_TIMEOUT_MS;
  console.log('run started');

  progress = setInterval(async () => {
    try {
      const s = await page.evaluate(() => ({
        status: document.getElementById('status').textContent,
        rows: document.getElementById('trace-count').textContent,
        stream: window.__stream ? { ...window.__stream } : null,
        exit: window.__lastExit,
      }));
      console.log(`  [${Math.round((Date.now() - runStart) / 1000)}s] ${s.status} rows=${s.rows} ` +
        `frames=${s.stream && s.stream.drawn} sent=${s.stream && s.stream.sent} ` +
        `presents=${s.stream && s.stream.presents} threads=${s.stream && s.stream.threads} ` +
        `input=${s.stream && s.stream.inputSent}/${s.stream && s.stream.inputDelivered} exit=${JSON.stringify(s.exit)}`);
    } catch { /* ignore */ }
  }, 20000);

  stage = 'stream attached';
  await page.waitForFunction(() => window.__stream && window.__stream.attached === true, null,
    { timeout: Math.max(1, deadline - Date.now()) });
  console.log('module reported the frame stream attached');

  stage = 'first frame while running';
  await page.waitForFunction(() => window.__stream && window.__stream.drawn >= 1 && !window.__lastExit, null,
    { timeout: Math.max(1, deadline - Date.now()) });
  const first = await page.evaluate(() => ({
    drawn: window.__stream.drawn,
    canvas: [document.getElementById('screen-canvas').width, document.getElementById('screen-canvas').height],
    running: document.getElementById('status').textContent,
  }));
  console.log(`first frame drawn while the guest is still running: ${first.drawn} frame(s), ` +
    `canvas ${first.canvas[0]}x${first.canvas[1]}, status "${first.running}"`);
  if (first.canvas[0] < 2) fail('the canvas was never sized, so no frame reached it');

  stage = 'inject input';
  const box = await page.locator('#screen-canvas').boundingBox();
  if (!box) fail('the canvas has no box to click');
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.up();
  await page.mouse.wheel(0, 120);
  await page.keyboard.press('KeyA');
  const sent = await page.evaluate(() => window.__stream.inputSent);
  console.log(`page queued ${sent} input event(s)`);
  if (sent < 4) fail(`only ${sent} events left the page`);

  stage = 'input reaches the emulator';
  await page.waitForFunction(() => window.__stream && window.__stream.inputDelivered > 0, null,
    { timeout: Math.max(1, deadline - Date.now()) });
  const delivery = await page.evaluate(() => ({
    delivered: window.__stream.inputDelivered,
    rows: [...document.querySelectorAll('#trace > *')].map((r) => r.textContent)
      .filter((t) => t.includes('reached the emulator')).slice(0, 6),
  }));
  console.log(`${delivery.delivered} event(s) reached the emulator; it reported each one:`);
  for (const row of delivery.rows) console.log(`    ${row}`);
  if (!delivery.rows.length) fail('the emulator never reported an input event by name');

  stage = 'more frames while running';
  await page.waitForFunction((before) => window.__stream && window.__stream.drawn > before, first.drawn,
    { timeout: Math.max(1, deadline - Date.now()) });
  console.log(`frame count advanced past ${first.drawn} during the run`);

  stage = 'guest finishes';
  await page.waitForFunction(() => window.__lastExit != null, null, { timeout: Math.max(1, deadline - Date.now()) });
  const done = await page.evaluate(() => ({ exit: window.__lastExit, stream: { ...window.__stream } }));
  console.log(`guest finished: ${JSON.stringify(done.exit)}`);
  console.log(`final counters: ${JSON.stringify(done.stream)}`);

  stage = 'pixels';
  const pixels = await page.evaluate(() => {
    const canvas = document.getElementById('screen-canvas');
    const data = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
    let nonBlack = 0;
    for (let i = 0; i < data.length; i += 4) {
      if (data[i] > 8 || data[i + 1] > 8 || data[i + 2] > 8) ++nonBlack;
    }
    return { width: canvas.width, height: canvas.height, total: canvas.width * canvas.height, nonBlack };
  });
  console.log(`canvas ${pixels.width}x${pixels.height}: ${pixels.nonBlack}/${pixels.total} non-black`);
  if (pixels.nonBlack < 10000) fail(`only ${pixels.nonBlack} non-black pixels`);

  stage = 'run torn down';
  await page.waitForFunction(() => document.getElementById('status').textContent !== 'running…', null, { timeout: 120000 });
  const finished = await page.evaluate(() => ({
    status: document.getElementById('status').textContent,
    runnable: !document.getElementById('run').disabled,
  }));
  console.log(`page settled: status "${finished.status}", run button re-enabled: ${finished.runnable}`);
  if (!finished.runnable) fail('the page never learned the run had ended');

  clearInterval(progress);
  await page.screenshot({ path: SHOT, fullPage: true });
  console.log(`screenshot saved to ${SHOT}`);

  // Backpressure. The main thread is held for one unbroken stretch longer than the rest of a run takes,
  // so not one ack is processed and the credit hits the floor; the module then has to refuse to compose
  // rather than queue.
  //
  // Two things the hold has to get right. It cannot start at the click: spawning the worker and fetching
  // the module are driven by the main thread, so a hold that begins there just postpones the run instead
  // of starving it -- the first frame is the proof that the worker is already going. And it cannot be
  // broken up to poll for the run's end, because the page drains its queue and acks in the gap, which
  // restores the credit and is exactly the case this is not testing.
  stage = 'backpressure';
  await page.fill('#opt-frame-interval', '1');
  await page.click('#run');
  await page.waitForFunction(() => window.__stream && window.__stream.drawn >= 1, null, { timeout: RUN_TIMEOUT_MS });

  await page.evaluate((ms) => {
    const until = Date.now() + ms;
    while (Date.now() < until) { /* the ack loop cannot run while this does */ }
  }, HOLD_MS);

  await page.waitForFunction(() => window.__lastExit != null, null, { timeout: 60000 });
  const fast = await page.evaluate(() => ({ ...window.__stream }));
  console.log(`page held for ${HOLD_MS / 1000} s at a 1 ms interval: ${fast.sent} sent, ${fast.drawn} drawn, ` +
    `${fast.dropped} refused, credit floor ${fast.credit}`);
  if (fast.credit !== 0) fail(`credit floor was ${fast.credit}: the run outlasted the hold, so the page was never starved`);
  if (!fast.dropped) fail('the module never refused a frame, so the credit is not holding anything back');

  if (consoleErrors.length) {
    console.log('console errors:');
    for (const line of consoleErrors) console.log(`  ${line}`);
  } else {
    console.log('no console errors');
  }

  if (done.stream.drawn < 2) fail(`only ${done.stream.drawn} frame(s) ever drawn`);
  if (done.exit.status !== 0) fail(`guest exited ${done.exit.status}`);

  console.log('PASS: frames streamed during the run, input reached the emulator, guest exited 0');
} catch (error) {
  code = 1;
  clearInterval(progress);
  console.error(`FAIL [${stage}]: ${error && error.message ? error.message.split('\n')[0] : error}`);
  try {
    const page = browser.contexts()[0].pages()[0];
    const dump = await page.evaluate(() => ({
      status: document.getElementById('status').textContent,
      exit: window.__lastExit,
      stream: window.__stream ? { ...window.__stream } : null,
      tail: [...document.querySelectorAll('#trace > *')].slice(-30).map((r) => r.textContent),
    }));
    console.error(`status=${dump.status} exit=${JSON.stringify(dump.exit)}`);
    console.error(`stream=${JSON.stringify(dump.stream)}`);
    console.error('trace tail:');
    for (const line of dump.tail) console.error(`  ${line}`);
    await page.screenshot({ path: SHOT, fullPage: true });
    console.error(`screenshot saved to ${SHOT}`);
  } catch { /* page may be gone */ }
  for (const line of consoleErrors) console.error(`  console: ${line}`);
} finally {
  await browser.close().catch(() => {});
}

process.exit(code);
