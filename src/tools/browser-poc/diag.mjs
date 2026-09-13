import { launchBrowser } from './browser.mjs';
import { writeFileSync } from 'node:fs';

const URL = process.env.POC_URL || 'http://127.0.0.1:8099/';

const browser = await launchBrowser('diag.mjs');
if (!browser) process.exit(0);

try {
  const page = await browser.newPage({ viewport: { width: 1400, height: 1000 } });
  page.on('pageerror', (error) => console.error('pageerror:', String(error)));
  page.on('response', (response) => {
    if (response.status() >= 400) console.error(`HTTP ${response.status()} ${response.url()}`);
  });

  await page.goto(URL, { waitUntil: 'load' });
  await page.waitForFunction(() => document.getElementById('status').textContent === 'ready', null, {
    timeout: 120000,
  });

  await page.click('#pick-dyld');
  await page.waitForFunction(() => document.getElementById('trace').textContent.includes('Attached'), null, {
    timeout: 60000,
  });

  await page.click('#add-paint-demo');
  await page.waitForFunction(() => document.getElementById('sel-path').textContent === '/bin/paint-demo', null, {
    timeout: 60000,
  });

  await page.click('#run');

  // The run either stops (deadlock) or exits. Wait for either, then dump.
  await page.waitForFunction(() => window.__lastExit != null, null, { timeout: 20 * 60 * 1000 });

  const dump = await page.evaluate(() => ({
    status: document.getElementById('status').textContent,
    exit: window.__lastExit,
    rows: [...document.querySelectorAll('#trace > *')].map((r) => r.textContent),
  }));

  writeFileSync('/tmp/wasm-trace.txt', dump.rows.join('\n') + '\n');
  console.log(`status=${dump.status}`);
  console.log(`exit=${JSON.stringify(dump.exit)}`);
  console.log(`rows=${dump.rows.length} -> /tmp/wasm-trace.txt`);
} finally {
  await browser.close().catch(() => {});
}
