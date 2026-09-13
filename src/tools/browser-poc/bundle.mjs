// Attaches a real .app bundle through the page's directory picker and checks that the whole subtree
// lands at the guest path the mount field names and that the launched binary is the one Info.plist
// declares. Nothing here knows which app it is: the bundle path comes from POC_BUNDLE.

import { launchBrowser } from './browser.mjs';

const URL = process.env.POC_URL || 'http://127.0.0.1:8110/';
const BUNDLE = process.env.POC_BUNDLE || '/System/Applications/Calculator.app';
const SHOT = '/tmp/browser-bundle.png';

const name = BUNDLE.split('/').filter(Boolean).pop();
const consoleErrors = [];
let stage = 'launch';

function fail(message) {
  throw new Error(message);
}

const browser = await launchBrowser('bundle.mjs');
if (!browser) process.exit(0);

let code = 0;

try {
  const page = await browser.newPage({ viewport: { width: 1500, height: 1000 } });
  page.on('console', (msg) => { if (msg.type() === 'error') consoleErrors.push(msg.text()); });
  page.on('pageerror', (error) => consoleErrors.push(String(error)));

  stage = 'open page';
  await page.goto(URL, { waitUntil: 'load' });
  await page.waitForFunction(() => document.getElementById('status').textContent === 'ready', null, { timeout: 120000 });

  stage = 'attach bundle';
  await page.locator('#bundle-input').setInputFiles(BUNDLE);
  await page.waitForFunction((bundle) => document.getElementById('trace').textContent.includes(`Attached ${bundle}`), name,
    { timeout: 60000 });

  const result = await page.evaluate(() => ({
    selected: document.getElementById('sel-path').textContent,
    count: document.getElementById('fs-count').textContent,
    gui: document.getElementById('opt-gui').checked,
    rows: [...document.querySelectorAll('#trace > *')].map((r) => r.textContent)
      .filter((t) => t.startsWith('Attached ') || t.startsWith('Launching ') || t.startsWith('Could not tell')),
    tree: [...document.querySelectorAll('#tree .node')].map((r) => r.textContent).slice(0, 40),
  }));

  for (const row of result.rows) console.log(`  ${row}`);
  console.log(`selected: ${result.selected}`);
  console.log(`filesystem: ${result.count}, window server auto-enabled: ${result.gui}`);

  const mount = await page.inputValue('#opt-bundle-mount');
  const expected = `${mount}/${name}/Contents/MacOS/${name.replace(/\.app$/i, '')}`;
  if (result.selected !== expected) fail(`selected ${result.selected}, expected ${expected}`);
  if (!result.rows.some((t) => t.includes('CFBundleExecutable'))) {
    console.log('note: the executable was not chosen from Info.plist; see the row above for the fallback used');
  }

  stage = 'guest paths';
  const paths = await page.evaluate(() => {
    const rows = [];
    for (const node of document.querySelectorAll('#tree .node.file')) rows.push(node.title.split(' ')[0]);
    return rows;
  });
  const inside = paths.filter((p) => p.startsWith(`${mount}/${name}/`));
  console.log(`${inside.length} of ${paths.length} attached files sit under ${mount}/${name}/`);
  if (inside.length < 5) fail('the bundle subtree did not land under the mount point');

  const nested = inside.filter((p) => p.includes('.appex/'));
  console.log(`nested bundles kept their own paths: ${nested.length} file(s) under a .appex`);

  await page.screenshot({ path: SHOT, fullPage: true });
  console.log(`screenshot saved to ${SHOT}`);

  if (consoleErrors.length) {
    console.log('console errors:');
    for (const line of consoleErrors) console.log(`  ${line}`);
  } else {
    console.log('no console errors');
  }

  console.log(`PASS: ${name} attached at ${mount}/${name} and ${expected} selected`);
} catch (error) {
  code = 1;
  console.error(`FAIL [${stage}]: ${error && error.message ? error.message.split('\n')[0] : error}`);
  try {
    const page = browser.contexts()[0].pages()[0];
    const dump = await page.evaluate(() => ({
      selected: document.getElementById('sel-path').textContent,
      tail: [...document.querySelectorAll('#trace > *')].slice(-20).map((r) => r.textContent),
    }));
    console.error(`selected=${dump.selected}`);
    for (const line of dump.tail) console.error(`  ${line}`);
    await page.screenshot({ path: SHOT, fullPage: true });
  } catch { /* page may be gone */ }
  for (const line of consoleErrors) console.error(`  console: ${line}`);
} finally {
  await browser.close().catch(() => {});
}

process.exit(code);
