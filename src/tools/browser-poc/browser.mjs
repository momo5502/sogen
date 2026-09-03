// The one place these drivers find a browser. playwright-core does not ship one, and every script here
// used to hardcode a path under one developer's ms-playwright cache, which made all of them unrunnable
// anywhere else. No executablePath and no channel: playwright-core resolves an already-installed browser
// through its own default registry lookup (the same one `npx playwright install` populates), which is
// how page/test/root-cache-adversarial.mjs finds one too.

import { chromium } from 'playwright-core';

// Null rather than a throw when nothing is installed: a missing browser is then a visible skip with an
// action attached, not a stack trace that reads like the page under test is broken.
export async function launchBrowser(script) {
  try {
    return await chromium.launch({ headless: true });
  } catch (error) {
    console.log(
      'SKIPPED: no compatible Chromium is installed for playwright-core ' +
        `(${error && error.message ? error.message.split('\n')[0] : error}). ` +
        `Run \`npx playwright install chromium\` and re-run \`node ${script}\`.`,
    );
    return null;
  }
}
