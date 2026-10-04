// Adversarial harness for page/public/macos-root-sw.js.
//
// This drives the real service worker inside a real browser against a small HTTP server this script
// owns (so response headers -- ETag, Last-Modified, Range support -- are exact and reproducible without
// a real macOS root). Cache Storage is manipulated directly from the page (`window.caches`), which is
// the same store the service worker reads, to inject corruption the network side of this server would
// never actually produce -- an evicted, truncated or empty block -- so the sw's own defensive checks
// are the thing under test, not this server.
//
// Usage: npm run test:root-cache (from page/), or node test/root-cache-adversarial.mjs directly.
// Exits non-zero on any failed assertion; exits 0 with a SKIPPED notice if no compatible browser is
// installed, so its absence is visible rather than the run silently never having happened.
// playwright-core is a page devDependency (see package.json) but does not itself ship a browser --
// `npx playwright install chromium` (or an existing install of the `playwright` package anywhere on
// the machine, which playwright-core's own resolution already checks) provides one.

import http from "node:http";
import { existsSync, statSync } from "node:fs";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright-core";
import ts from "typescript";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SW_PATH = path.join(HERE, "..", "public", "macos-root-sw.js");
const DIST_DIR = path.join(HERE, "..", "dist");
const SRC_MACOS_DIR = path.join(HERE, "..", "src", "macos");

// Apart from Installer (stubbed below) root-cache.ts and the served-listing module it imports use only
// browser globals, so a type-stripping transpile is enough to run them as real ES modules in the
// browser -- no bundler, no module resolution, and no risk of testing a hand-reimplemented stand-in
// instead of the files that ship. Served under the names the import specifiers resolve to: a module at
// /root-cache.js reaches "./served-listing" at /served-listing and "../Installer" at /Installer.
const PAGE_MODULES = {
  "/root-cache.js": "root-cache.ts",
  "/served-listing": "served-listing.ts",
};

async function transpilePageModules() {
  const modules = {};

  for (const [route, file] of Object.entries(PAGE_MODULES)) {
    const source = await readFile(path.join(SRC_MACOS_DIR, file), "utf8");
    modules[route] = ts.transpileModule(source, {
      compilerOptions: {
        module: ts.ModuleKind.ESNext,
        target: ts.ScriptTarget.ES2020,
      },
    }).outputText;
  }

  return modules;
}

const BLOCK = 2 * 1024 * 1024;
const FILE_SIZE = 3 * BLOCK + 654321;

function byteAt(i) {
  return (i * 37 + 11) % 256;
}

function fillDeterministic(size, salt) {
  const buffer = Buffer.allocUnsafe(size);
  for (let i = 0; i < size; ++i) buffer[i] = (byteAt(i) + salt) & 0xff;
  return buffer;
}

const MAIN_FIXTURE = fillDeterministic(FILE_SIZE, 0);

// Both sized past one whole block so block 0 is the same BLOCK-length span for each file: with the size
// left out of the key, the sw's own "is this block the length its position implies" check cannot tell
// the collision apart from a genuine hit, because a full block is a full block either way. Only the
// *content* differs, which is exactly the corruption an unkeyed collision produces.
const COLLIDE_A = fillDeterministic(BLOCK + 111, 5);
const COLLIDE_B = fillDeterministic(BLOCK + 222, 91);
const SHARED_LAST_MODIFIED = "Wed, 01 Jan 2020 00:00:00 GMT";

// Two different files behind one validator, which is what serve.py used to hand out: it derived the
// ETag from size and mtime, and a sealed macOS system volume stamps every file with the same mtime, so
// the ETag was the size. 1,338 bytes is the length at which CoreUI.framework's Info.plist collides with
// DeveloperToolsSupport.framework's on a macOS 26 host -- same length, different CFBundleExecutable --
// and reading one returned the other at exactly the right length, so no short-read guard here fires.
const TWIN_SIZE = 1338;
const TWIN_ETAG = '"53a-18c7d18167e8fe00"';
const TWIN_A = fillDeterministic(TWIN_SIZE, 17);
const TWIN_B = fillDeterministic(TWIN_SIZE, 200);

const FIXTURES = {
  "main.bin": {
    data: MAIN_FIXTURE,
    etag: '"main-v1"',
    lastModified: "Thu, 02 Jan 2020 00:00:00 GMT",
  },
  "collide-a.bin": {
    data: COLLIDE_A,
    etag: null,
    lastModified: SHARED_LAST_MODIFIED,
  },
  "collide-b.bin": {
    data: COLLIDE_B,
    etag: null,
    lastModified: SHARED_LAST_MODIFIED,
  },
  "twin-a.bin": {
    data: TWIN_A,
    etag: TWIN_ETAG,
    lastModified: SHARED_LAST_MODIFIED,
  },
  "twin-b.bin": {
    data: TWIN_B,
    etag: TWIN_ETAG,
    lastModified: SHARED_LAST_MODIFIED,
  },
};

// root-cache.ts leaves registration to page/src/Installer.ts, which imports the Vite PWA virtual module
// and the React-based Loader -- neither of which resolves outside a bundle. This stands in for it with
// the two behaviours registerRootCache() depends on and that the real Installer.ensureServiceWorker()
// provides: it rejects when registering fails, and a later call retries instead of handing back the
// same rejection. `../Installer` from a module served at /root-cache.js resolves to /Installer.
const INSTALLER_STUB = [
  "let registration = null;",
  "",
  "export default {",
  "  ensureServiceWorker() {",
  "    if (!registration) {",
  "      const attempt = navigator.serviceWorker",
  "        .register('/macos-root-sw.js', { scope: '/' })",
  "        .then(() => undefined);",
  "      attempt.catch(() => { registration = null; });",
  "      registration = attempt;",
  "    }",
  "    return registration;",
  "  },",
  "};",
].join("\n");

function parseRange(header, total) {
  const match = /^bytes=(\d+)-(\d+)$/.exec((header || "").trim());
  if (!match) return null;
  const start = Number(match[1]);
  const end = Math.min(Number(match[2]), total - 1);
  if (start > end || start >= total) return null;
  return { start, end };
}

function startServer(swSource, pageModules) {
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, "http://localhost");

    if (url.pathname === "/") {
      res.writeHead(200, { "Content-Type": "text/html" });
      res.end(
        "<!doctype html><title>root-cache-adversarial</title><body>ready</body>",
      );
      return;
    }

    if (url.pathname === "/macos-root-sw.js") {
      res.writeHead(200, { "Content-Type": "text/javascript" });
      res.end(swSource);
      return;
    }

    if (pageModules && Object.hasOwn(pageModules, url.pathname)) {
      res.writeHead(200, { "Content-Type": "text/javascript" });
      res.end(pageModules[url.pathname]);
      return;
    }

    if (url.pathname === "/Installer" && pageModules) {
      res.writeHead(200, { "Content-Type": "text/javascript" });
      res.end(INSTALLER_STUB);
      return;
    }

    const match = /^\/macos-root\/(.+)$/.exec(url.pathname);
    const fixture = match ? FIXTURES[match[1]] : null;

    if (!fixture) {
      res.writeHead(404);
      res.end();
      return;
    }

    const headers = {
      "Content-Type": "application/octet-stream",
      "Last-Modified": fixture.lastModified,
      "Accept-Ranges": "bytes",
    };
    if (fixture.etag) headers["ETag"] = fixture.etag;

    if (req.method === "HEAD") {
      res.writeHead(200, {
        ...headers,
        "Content-Length": String(fixture.data.length),
      });
      res.end();
      return;
    }

    const range = req.headers["range"]
      ? parseRange(req.headers["range"], fixture.data.length)
      : null;

    if (!range) {
      res.writeHead(200, {
        ...headers,
        "Content-Length": String(fixture.data.length),
      });
      res.end(fixture.data);
      return;
    }

    const body = fixture.data.subarray(range.start, range.end + 1);
    res.writeHead(206, {
      ...headers,
      "Content-Length": String(body.length),
      "Content-Range": `bytes ${range.start}-${range.end}/${fixture.data.length}`,
    });
    res.end(body);
  });

  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

// Runs the whole adversarial suite once against one served copy of the service worker, in a fresh
// browser context on its own origin (a distinct port makes it a distinct origin, which is what gives
// every run a clean Cache Storage and service-worker registration without reaching into either API to
// tear them down by hand).
// A worker script embedded as a string rather than a separate file: the whole point is to run it from
// inside a real dedicated Worker, so a Blob URL is enough and keeps every sub-case below self-contained.
const RANGE_WORKER_SOURCE = [
  "self.onmessage = async (e) => {",
  "  const { url, start, end } = e.data;",
  "  try {",
  "    const response = await fetch(url, { headers: { Range: `bytes=${start}-${end}` } });",
  "    const body = new Uint8Array(await response.arrayBuffer());",
  "    self.postMessage({",
  "      status: response.status,",
  "      cacheHeader: response.headers.get('X-Sogen-Cache'),",
  "      length: body.length,",
  "    });",
  "  } catch (error) {",
  "    self.postMessage({ error: String(error) });",
  "  }",
  "};",
].join("\n");

async function rangeWorkerRequest(
  page,
  mainUrl,
  awaitRegister,
  awaitControllerchange,
) {
  return page.evaluate(
    async ({ mainUrl, awaitRegister, awaitControllerchange, workerSource }) => {
      function makeRangeWorker() {
        return new Worker(
          URL.createObjectURL(
            new Blob([workerSource], { type: "text/javascript" }),
          ),
        );
      }

      function askWorker(worker, message) {
        return new Promise((resolve) => {
          worker.addEventListener("message", (event) => resolve(event.data), {
            once: true,
          });
          worker.postMessage(message);
        });
      }

      const registration = navigator.serviceWorker.register(
        "/macos-root-sw.js",
        { scope: "/" },
      );
      if (awaitRegister) {
        await registration;
      }
      if (awaitControllerchange && !navigator.serviceWorker.controller) {
        await new Promise((resolve) => {
          navigator.serviceWorker.addEventListener(
            "controllerchange",
            () => resolve(),
            { once: true },
          );
        });
      }

      const worker = makeRangeWorker();
      const result = await askWorker(worker, {
        url: mainUrl,
        start: 0,
        end: 999,
      });
      worker.terminate();
      return result;
    },
    {
      mainUrl,
      awaitRegister,
      awaitControllerchange,
      workerSource: RANGE_WORKER_SOURCE,
    },
  );
}

async function freshPage(browser, mainUrl) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  await page.goto(mainUrl.split("/macos-root/")[0] + "/");
  return { ctx, page };
}

// The exact race the whole clients.claim()/controllerchange design exists to avoid: a dedicated
// Worker's controller is fixed at the moment it is created, from whatever controls its document right
// then -- it is not reassigned later when the document is claimed. page/public/macos-root-sw.js stamps
// X-Sogen-Cache: block on every response it constructs itself, cache hit or fill, so that header's
// absence is exactly "this request was never seen by the service worker"; its presence is exactly "the
// service worker answered this", which is the assertion clients.claim()/controllerchange exist to make
// true for a worker created after registerRootCache() resolves.
//
// Each case gets its own fresh browser context (so its own fresh, unregistered origin) rather than
// reusing one page across cases: reusing the page would let an earlier case's registration finish
// registering for real by the time a later case runs, which is exactly the timing this is trying to
// pin down rather than leave to how fast Playwright's own round trips happen to be.
async function testWorkerControl(browser, mainUrl) {
  const results = [];

  {
    const { ctx, page } = await freshPage(browser, mainUrl);
    const notControlled = await rangeWorkerRequest(page, mainUrl, false, false);
    await ctx.close();
    results.push({
      name: "a Worker created before controllerchange bypasses the cache",
      pass:
        notControlled.status === 206 && notControlled.cacheHeader !== "block",
      detail: `status=${notControlled.status} X-Sogen-Cache=${notControlled.cacheHeader}`,
    });
  }

  {
    const { ctx, page } = await freshPage(browser, mainUrl);
    const controlled = await rangeWorkerRequest(page, mainUrl, true, true);
    await ctx.close();
    results.push({
      name: "a Worker created after registerRootCache()'s controllerchange wait is answered by the service worker",
      pass: controlled.status === 206 && controlled.cacheHeader === "block",
      detail: `status=${controlled.status} X-Sogen-Cache=${controlled.cacheHeader}`,
    });
  }

  // A third case -- awaiting register() itself but skipping the controllerchange check -- was tried
  // here and dropped: register()'s promise is documented to resolve once the registration exists, not
  // once install/activate/clients.claim() finish, but measured across repeated worktree runs that gap
  // was sometimes wide enough for a same-tick worker to bypass the cache and sometimes not (a warm
  // script/code cache from an earlier context in the same run measurably speeds up a later context's
  // install). A test that can fail on unchanged, correct code is worse than no test; the two cases
  // above already answer the question this file exists to answer without depending on that race.

  return results;
}

// Regression guard for the bypass flag's storage. Two things have to survive that a single
// page.evaluate() cannot exercise on its own: sogen-cache-clear deleting CACHE wholesale (the flag must
// not have been living inside it), and the browser recycling the worker between visits, which resets
// every top-level `let` -- including the in-memory mirror -- and forces the answer to come from Cache
// Storage alone. The second one needs the worker actually torn down and restarted, which only CDP can
// force from outside the page; ServiceWorker.stopAllWorkers does that without touching the registration.
async function testBypassSurvivesRestart(browser, mainUrl) {
  const base = mainUrl.split("/macos-root/")[0];
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.goto(base + "/");
  const results = [];

  await page.evaluate(async () => {
    const registration = await navigator.serviceWorker.register(
      "/macos-root-sw.js",
      { scope: "/" },
    );
    if (!navigator.serviceWorker.controller) {
      await new Promise((resolve) => {
        navigator.serviceWorker.addEventListener(
          "controllerchange",
          () => resolve(),
          { once: true },
        );
      });
    }
    return registration.scope;
  });

  const swAsk = (message) =>
    page.evaluate((message) => {
      const controller = navigator.serviceWorker.controller;
      if (!controller) return Promise.reject(new Error("no controller"));
      const id = Math.random();
      return new Promise((resolve, reject) => {
        const onMessage = (event) => {
          if (event.data && event.data.id === id) {
            navigator.serviceWorker.removeEventListener("message", onMessage);
            resolve(event.data);
          }
        };
        navigator.serviceWorker.addEventListener("message", onMessage);
        controller.postMessage({ ...message, id });
        setTimeout(() => reject(new Error("sw did not answer")), 20000);
      });
    }, message);

  const rangedFetch = () =>
    page.evaluate(
      (url) =>
        fetch(url, { headers: { Range: "bytes=0-15" } }).then((r) => ({
          status: r.status,
          cacheHeader: r.headers.get("X-Sogen-Cache"),
        })),
      mainUrl,
    );

  const before = await rangedFetch();
  results.push({
    name: "bypass off by default: a ranged read is served from the cache",
    pass: before.cacheHeader === "block",
    detail: JSON.stringify(before),
  });

  await swAsk({ t: "sogen-cache-bypass", enabled: true });
  const onStatus = await swAsk({ t: "sogen-cache-status" });
  results.push({
    name: "sogen-cache-status reports bypassed after turning it on",
    pass: onStatus.bypassed === true,
    detail: JSON.stringify(onStatus),
  });

  const whileOn = await rangedFetch();
  results.push({
    name: "bypass on: a ranged read is not served from the cache",
    pass: whileOn.status === 206 && whileOn.cacheHeader !== "block",
    detail: JSON.stringify(whileOn),
  });

  await swAsk({ t: "sogen-cache-clear" });
  const afterClearStatus = await swAsk({ t: "sogen-cache-status" });
  results.push({
    name: "Clear while bypassed leaves bypass on, same worker instance",
    pass: afterClearStatus.bypassed === true,
    detail: JSON.stringify(afterClearStatus),
  });

  // The bug this guards: bypassKnown (memory) trivially survives Clear regardless of where the
  // persisted flag lived, so the checks above pass even when the flag itself was erased. Only a worker
  // the browser actually restarts re-derives its answer from Cache Storage alone -- which is exactly
  // where the flag being stored inside CACHE showed up as a bug: Clear had already deleted it, so a
  // fresh worker found nothing and silently went back to caching.
  const cdp = await context.newCDPSession(page);
  await cdp.send("ServiceWorker.enable");
  await cdp.send("ServiceWorker.stopAllWorkers");
  await new Promise((resolve) => setTimeout(resolve, 500));

  const afterRestartStatus = await swAsk({ t: "sogen-cache-status" });
  results.push({
    name: "bypass survives Clear across a worker restart",
    pass: afterRestartStatus.bypassed === true,
    detail: JSON.stringify(afterRestartStatus),
  });

  const afterRestartFetch = await rangedFetch();
  results.push({
    name: "a ranged read after Clear + restart is still not served from the cache",
    pass:
      afterRestartFetch.status === 206 &&
      afterRestartFetch.cacheHeader !== "block",
    detail: JSON.stringify(afterRestartFetch),
  });

  await swAsk({ t: "sogen-cache-bypass", enabled: false });
  const restored = await rangedFetch();
  results.push({
    name: "turning bypass back off restores caching",
    pass: restored.cacheHeader === "block",
    detail: JSON.stringify(restored),
  });

  await context.close();
  return results;
}

// Deterministic by construction rather than by luck: navigator.serviceWorker.register is overridden to
// reject exactly once, so this does not depend on any real browser timing the way a genuine first-visit
// failure would -- the point is registerRootCache()'s own retry logic, not reproducing a rare condition.
// Runs the real page/src/macos/root-cache.ts (type-stripped, not reimplemented) so this exercises the
// shipped memoization rather than a stand-in for it; only Installer is stubbed, and the failure still
// arrives through the same navigator.serviceWorker.register it arrives through in the app.
async function testRegistrationRetry(browser, base) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  await page.goto(base + "/");

  const result = await page.evaluate(async () => {
    let calls = 0;
    const original = navigator.serviceWorker.register.bind(
      navigator.serviceWorker,
    );
    navigator.serviceWorker.register = (...args) => {
      calls += 1;
      if (calls === 1)
        return Promise.reject(new Error("forced failure for test"));
      return original(...args);
    };

    const { registerRootCache } = await import("/root-cache.js");

    let firstError = null;
    try {
      await registerRootCache();
    } catch (error) {
      firstError = error && error.message ? error.message : String(error);
    }

    let secondError = null;
    let secondState = null;
    try {
      secondState = await registerRootCache();
    } catch (error) {
      secondError = error && error.message ? error.message : String(error);
    }

    return { calls, firstError, secondError, secondState };
  });

  await ctx.close();
  return result;
}

// --- the built app, not the sw in isolation ---------------------------------------------------------
//
// Everything above serves one hand-written service worker on a bare page. The failure this section
// guards against only exists in the assembled app: two registrations at scope "/" with different script
// URLs, which the spec collapses into one -- so whichever registered last evicts the other, and
// Installer's controllerchange reload turns that into an unbounded navigation loop. Nothing short of the
// real dist/ (real Installer, real Workbox sw.js, real precache manifest) can show that.

const DIST_MIME = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".png": "image/png",
  ".svg": "image/svg+xml",
  ".wasm": "application/wasm",
  ".webmanifest": "application/manifest+json",
  ".woff": "font/woff",
  ".woff2": "font/woff2",
};

// dist/ ships no /macos-root/, and without one there is no way to tell whether the range logic is still
// reachable once it stops being its own registration.
const PROBE = fillDeterministic(2 * BLOCK + 4096, 17);
const PROBE_LAST_MODIFIED = "Fri, 03 Jan 2020 00:00:00 GMT";
const PROBE_PATH = "/macos-root/probe.bin";

const LOOP_WINDOW_MS = 8000;

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function startDistServer() {
  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, "http://localhost");

    if (url.pathname === PROBE_PATH) {
      const headers = {
        "Content-Type": "application/octet-stream",
        "Last-Modified": PROBE_LAST_MODIFIED,
        "Accept-Ranges": "bytes",
      };

      if (req.method === "HEAD") {
        res.writeHead(200, {
          ...headers,
          "Content-Length": String(PROBE.length),
        });
        res.end();
        return;
      }

      const range = req.headers["range"]
        ? parseRange(req.headers["range"], PROBE.length)
        : null;

      if (!range) {
        res.writeHead(200, {
          ...headers,
          "Content-Length": String(PROBE.length),
        });
        res.end(PROBE);
        return;
      }

      const body = PROBE.subarray(range.start, range.end + 1);
      res.writeHead(206, {
        ...headers,
        "Content-Length": String(body.length),
        "Content-Range": `bytes ${range.start}-${range.end}/${PROBE.length}`,
      });
      res.end(body);
      return;
    }

    const relative = url.pathname === "/" ? "/index.html" : url.pathname;
    const file = path.join(DIST_DIR, decodeURIComponent(relative));

    if (!file.startsWith(DIST_DIR + path.sep)) {
      res.writeHead(403);
      res.end();
      return;
    }

    let body;
    try {
      body = await readFile(file);
    } catch {
      res.writeHead(404);
      res.end();
      return;
    }

    res.writeHead(200, {
      "Content-Type":
        DIST_MIME[path.extname(file)] || "application/octet-stream",
      "Content-Length": String(body.length),
    });
    res.end(body);
  });

  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

// A page under test may navigate at any moment -- that is the whole point of the loop case -- and an
// evaluate whose execution context is torn down mid-call rejects. Retrying is what makes these reads
// report the state of the app rather than the state of the race.
async function retryEvaluate(page, fn, arg, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastError;

  for (;;) {
    try {
      return await page.evaluate(fn, arg);
    } catch (error) {
      lastError = error;
      if (Date.now() > deadline) {
        throw lastError;
      }
      await sleep(150);
    }
  }
}

// Injected rather than repeated: every read of the registration list wants the same shape, and the
// page under test may be mid-navigation when it is asked. Assigned onto window because Playwright
// evaluates an init script inside a function scope, where a bare declaration would stay local to it.
const SCRIPT_PATHS_HELPER = `
  window.scriptPaths = async () => {
    const found = await navigator.serviceWorker.getRegistrations();
    return found.map((entry) => {
      const worker = entry.active || entry.waiting || entry.installing;
      return worker ? new URL(worker.scriptURL).pathname : "none";
    });
  };
`;

async function waitForRegistration(page, timeoutMs) {
  const deadline = Date.now() + timeoutMs;

  for (;;) {
    let scripts = [];
    try {
      scripts = await page.evaluate(async () => scriptPaths());
    } catch {
      /* navigated out from under the evaluate */
    }

    // A registration shows up in the list before any of installing/waiting/active is populated, so
    // stopping at the first non-empty list samples it as "none" rather than as the script it is.
    if (
      (scripts.length && !scripts.includes("none")) ||
      Date.now() > deadline
    ) {
      return scripts;
    }

    await sleep(150);
  }
}

async function waitForController(page, timeoutMs) {
  const deadline = Date.now() + timeoutMs;

  while (Date.now() < deadline) {
    try {
      if (await page.evaluate(() => !!navigator.serviceWorker.controller)) {
        return true;
      }
    } catch {
      /* navigated out from under the evaluate */
    }
    await sleep(150);
  }

  return false;
}

async function gotoTolerant(page, url) {
  try {
    await page.goto(url);
  } catch {
    /* a service worker reload can abort the navigation we asked for; waitForController settles it */
  }
}

// reload(), not a second goto() to the same URL: the app lives behind a HashRouter, so navigating to
// the same "/#/playground" is a same-document fragment navigation and never re-runs Installer -- which
// is exactly the visit the loop needs.
async function reloadTolerant(page) {
  try {
    await page.reload();
  } catch {
    /* same as above */
  }
}

const INSTALL_PROMPT = "Install Sogen for offline use?";

// Registering the worker for macOS mode must not turn every other visitor into a PWA user. Upstream
// windows and linux visitors who never accepted the prompt had no service worker and no precache
// before this branch, and this is what holds that line -- the assertion is a negative, so it is taken
// after a settle window long enough for a registration to have happened if one were going to.
async function testInstallStaysOptIn(browser, base) {
  const ctx = await browser.newContext({ baseURL: base });
  const results = [];

  try {
    await ctx.addInitScript({ content: SCRIPT_PATHS_HELPER });
    const page = await ctx.newPage();
    await gotoTolerant(page, `${base}/#/playground`);
    await page.getByText(INSTALL_PROMPT).waitFor({ timeout: 30000 });
    await sleep(5000);

    const untouched = await page.evaluate(async () => {
      const registrations = await navigator.serviceWorker.getRegistrations();
      const caches = await window.caches.keys();
      return {
        registrations: registrations.length,
        precaches: caches.filter((name) => name.startsWith("workbox-precache"))
          .length,
      };
    });

    results.push({
      name: "a visitor who never installed gets no worker and no precache",
      pass: untouched.registrations === 0 && untouched.precaches === 0,
      detail: `${untouched.registrations} registration(s), ${untouched.precaches} precache(s)`,
    });

    await page.getByRole("button", { name: "Yes", exact: true }).click();

    const afterInstall = await waitForRegistration(page, 30000);
    results.push({
      name: "accepting the install prompt registers the Workbox worker",
      pass: afterInstall.length === 1 && afterInstall[0] === "/sw.js",
      detail: afterInstall.join(", ") || "none",
    });

    await reloadTolerant(page);
    await sleep(3000);

    const afterReload = await retryEvaluate(
      page,
      async (prompt) => ({
        scripts: await scriptPaths(),
        prompt: document.body.innerText.includes(prompt),
      }),
      INSTALL_PROMPT,
      20000,
    ).catch(() => null);

    results.push({
      name: "an installed visitor keeps the worker and stops being asked",
      pass:
        !!afterReload &&
        afterReload.scripts.length === 1 &&
        afterReload.scripts[0] === "/sw.js" &&
        !afterReload.prompt,
      detail: afterReload
        ? `${afterReload.scripts.join(", ") || "none"}, prompt shown=${afterReload.prompt}`
        : "the page never settled",
    });
  } finally {
    await ctx.close();
  }

  return results;
}

async function testBuiltApp(browser) {
  if (
    !existsSync(path.join(DIST_DIR, "index.html")) ||
    !existsSync(path.join(DIST_DIR, "sw.js"))
  ) {
    return [
      {
        name: "one service worker at / after a macOS-mode visit",
        pass: false,
        skipped: true,
        detail:
          `no build in ${DIST_DIR} -- run \`npm run build\`, then re-run ` +
          "`npm run test:root-cache`",
      },
    ];
  }

  const wasmPath = path.join(DIST_DIR, "macos-analyzer.wasm");
  const wasmSize = existsSync(wasmPath) ? statSync(wasmPath).size : 0;

  const server = await startDistServer();
  const { port } = server.address();
  const base = `http://127.0.0.1:${port}`;
  const ctx = await browser.newContext({ baseURL: base });
  await ctx.addInitScript({ content: SCRIPT_PATHS_HELPER });
  const results = [];

  try {
    results.push(...(await testInstallStaysOptIn(browser, base)));

    const page = await ctx.newPage();

    // macOS mode has no UI entry point yet; the setting it will flip is the one the playground already
    // reads on mount, and flipping it here is what makes MacosGuestScreen mount and call
    // registerRootCache() -- the real product path rather than a registration this test hand-rolls,
    // which would keep "reproducing" the bug long after the product stopped causing it.
    await page.addInitScript(() => {
      localStorage.setItem("settings", JSON.stringify({ mode: "macos" }));
    });

    await gotoTolerant(page, `${base}/#/playground`);
    await waitForController(page, 30000);

    // A cold visit cannot loop: with nothing registered yet, Installer's probe finds nothing to
    // reinstall. The loop needs the *second* visit, whose probe finds whatever the first one left.
    await reloadTolerant(page);
    await waitForController(page, 30000);

    let navigations = 0;
    const countNavigation = (frame) => {
      if (frame === page.mainFrame()) {
        ++navigations;
      }
    };

    page.on("framenavigated", countNavigation);
    await sleep(LOOP_WINDOW_MS);
    page.off("framenavigated", countNavigation);

    results.push({
      name: "a warm macOS-mode visit does not reload in a loop",
      pass: navigations <= 1,
      detail: `${navigations} main-frame navigation(s) in ${LOOP_WINDOW_MS / 1000}s`,
    });

    try {
      const scripts = await retryEvaluate(
        page,
        async () => scriptPaths(),
        undefined,
        20000,
      );

      results.push({
        name: "exactly one service worker registration, and it is the Workbox one",
        pass: scripts.length === 1 && scripts[0] === "/sw.js",
        detail: `${scripts.length} registration(s): ${scripts.join(", ") || "none"}`,
      });
    } catch (error) {
      results.push({
        name: "exactly one service worker registration, and it is the Workbox one",
        pass: false,
        detail: String(error),
      });
    }

    try {
      const answer = await retryEvaluate(
        page,
        async ({ workerSource, url }) => {
          const worker = new Worker(
            URL.createObjectURL(
              new Blob([workerSource], { type: "text/javascript" }),
            ),
          );
          const reply = await new Promise((resolve) => {
            worker.addEventListener("message", (event) => resolve(event.data), {
              once: true,
            });
            worker.postMessage({ url, start: 0, end: 999 });
          });
          worker.terminate();
          return reply;
        },
        { workerSource: RANGE_WORKER_SOURCE, url: `${base}${PROBE_PATH}` },
        20000,
      );

      results.push({
        name: "the one worker still answers a macOS range read from inside a Worker",
        pass:
          answer.status === 206 &&
          answer.cacheHeader === "block" &&
          answer.length === 1000,
        detail: `status=${answer.status} X-Sogen-Cache=${answer.cacheHeader} length=${answer.length}`,
      });
    } catch (error) {
      results.push({
        name: "the one worker still answers a macOS range read from inside a Worker",
        pass: false,
        detail: String(error),
      });
    }

    // The windows/linux regression this whole section exists to close: a macOS-mode visit must not cost
    // every other mode its offline support. Asserted directly rather than inferred from the
    // registration count, because the precache surviving in Cache Storage is not the same thing as it
    // still being reachable.
    await ctx.setOffline(true);
    try {
      const offline = await retryEvaluate(
        page,
        async () => {
          const read = async (url) => {
            try {
              const response = await fetch(url);
              if (!response.ok) {
                return { ok: false, detail: `status ${response.status}` };
              }
              return {
                ok: true,
                length: (await response.arrayBuffer()).byteLength,
              };
            } catch (error) {
              return { ok: false, detail: String(error) };
            }
          };

          return {
            index: await read("/index.html"),
            wasm: await read("/macos-analyzer.wasm"),
          };
        },
        undefined,
        20000,
      );

      results.push({
        name: "offline: /index.html is still precached after a macOS-mode visit",
        pass: offline.index.ok,
        detail: offline.index.ok
          ? `${offline.index.length} bytes`
          : offline.index.detail,
      });
      results.push({
        name: "offline: /macos-analyzer.wasm is still precached after a macOS-mode visit",
        pass: offline.wasm.ok && offline.wasm.length === wasmSize,
        detail: offline.wasm.ok
          ? `${offline.wasm.length} bytes (expected ${wasmSize})`
          : offline.wasm.detail,
      });
    } catch (error) {
      results.push({
        name: "offline: the precache is still reachable after a macOS-mode visit",
        pass: false,
        detail: String(error),
      });
    } finally {
      await ctx.setOffline(false);
    }
  } finally {
    await ctx.close();
    await new Promise((resolve) => server.close(resolve));
  }

  return results;
}

async function runSuite(swSource, browser, pageModules) {
  const server = await startServer(swSource, pageModules);
  const { port } = server.address();
  const base = `http://127.0.0.1:${port}`;

  const context = await browser.newContext({ baseURL: base });
  const page = await context.newPage();
  const consoleErrors = [];
  page.on("pageerror", (error) => consoleErrors.push(String(error)));

  const results = [];

  try {
    await page.goto(base + "/");

    results.push(
      ...(await testWorkerControl(browser, `${base}/macos-root/main.bin`)),
    );

    results.push(
      ...(await testBypassSurvivesRestart(
        browser,
        `${base}/macos-root/main.bin`,
      )),
    );

    if (pageModules) {
      const retry = await testRegistrationRetry(browser, base);
      results.push({
        name: "registerRootCache() surfaces a failed register() to its caller",
        pass: !!retry.firstError && retry.firstError.includes("forced failure"),
        detail: `firstError=${retry.firstError}`,
      });
      results.push({
        name: "registerRootCache() retries (does not memoize a rejection) and succeeds",
        pass: retry.calls === 2 && !retry.secondError && !!retry.secondState,
        detail: `calls=${retry.calls} secondError=${retry.secondError} state=${JSON.stringify(retry.secondState)}`,
      });
    }

    // The worker-control cases above each ran in their own throwaway context; this page (used for
    // every case below) still needs its own registration and controller before any of them can ask the
    // service worker anything.
    await page.evaluate(async () => {
      const registration = await navigator.serviceWorker.register(
        "/macos-root-sw.js",
        { scope: "/" },
      );
      if (!navigator.serviceWorker.controller) {
        await new Promise((resolve) => {
          navigator.serviceWorker.addEventListener(
            "controllerchange",
            () => resolve(),
            { once: true },
          );
        });
      }
      return registration.scope;
    });

    const suiteResults = await page.evaluate(
      async ({ block, fileSize, twinSize }) => {
        const out = [];
        const record = (name, pass, detail) =>
          out.push({ name, pass, detail: detail || "" });

        function byteAt(i) {
          return (i * 37 + 11) % 256;
        }

        function expectedBytes(size, salt, start, length) {
          const buf = new Uint8Array(length);
          for (let i = 0; i < length; ++i)
            buf[i] = (byteAt(start + i) + salt) & 0xff;
          return buf;
        }

        function bytesEqual(a, b) {
          if (a.length !== b.length) return false;
          for (let i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false;
          return true;
        }

        async function readRange(url, start, length) {
          const response = await fetch(url, {
            headers: { Range: `bytes=${start}-${start + length - 1}` },
          });
          if (response.status !== 206)
            throw new Error(`expected 206, got ${response.status}`);
          const body = new Uint8Array(await response.arrayBuffer());
          const range = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(
            response.headers.get("Content-Range") || "",
          );
          if (!range) throw new Error("no Content-Range on a 206");
          return { body, total: Number(range[3]) };
        }

        function swAsk(message) {
          const controller = navigator.serviceWorker.controller;
          if (!controller) return Promise.reject(new Error("no controller"));
          const id = Math.random();
          return new Promise((resolve, reject) => {
            const onMessage = (event) => {
              if (event.data && event.data.id === id) {
                navigator.serviceWorker.removeEventListener(
                  "message",
                  onMessage,
                );
                resolve(event.data);
              }
            };
            navigator.serviceWorker.addEventListener("message", onMessage);
            controller.postMessage({ ...message, id });
            setTimeout(() => reject(new Error("sw did not answer")), 20000);
          });
        }

        function blockKey(cacheOrigin, pathname, id, etag, size, index) {
          const scope = id ? `id=${id}` : `at=${pathname}`;
          const key = `${scope}|${etag}:${size}`;
          return new Request(
            `${cacheOrigin}/__sogen-block/${encodeURIComponent(key)}/${index}`,
          );
        }

        const CACHE_NAME = "sogen-macos-root-v5";
        const origin = location.origin;
        const mainUrl = `${origin}/macos-root/main.bin`;

        // --- eight awkward ranges, byte-exact -------------------------------------------------------
        const ranges = [
          ["first byte", 0, 1],
          ["inside one block", 5000, 1000],
          ["straddling two blocks", block - 100, 200],
          ["exactly one whole block", 2 * block, block],
          ["two-byte read", 12345, 2],
          ["last 32 bytes of file", fileSize - 32, 32],
          ["BLOCK+4321 at an odd offset", block + 4321, 777],
          ["spanning three blocks", block - 10, 2 * block + 20],
        ];

        for (const [name, start, length] of ranges) {
          try {
            const { body } = await readRange(mainUrl, start, length);
            const expected = expectedBytes(fileSize, 0, start, length);
            record(
              `range: ${name}`,
              bytesEqual(body, expected),
              `got ${body.length} bytes`,
            );
          } catch (error) {
            record(`range: ${name}`, false, String(error));
          }
        }

        // --- evicted block: deleted from Cache Storage, must refill correctly -----------------------
        try {
          await readRange(mainUrl, 10, 10); // ensure block 0 is cached
          const status1 = await swAsk({ t: "sogen-cache-status" });
          const cache = await caches.open(CACHE_NAME);
          const keys = await cache.keys();
          const blockKeyForFile = keys.find(
            (k) => k.url.includes("/__sogen-block/") && k.url.endsWith("/0"),
          );
          if (!blockKeyForFile) throw new Error("block 0 was never cached");
          await cache.delete(blockKeyForFile);

          const { body } = await readRange(mainUrl, 10, 10);
          const expected = expectedBytes(fileSize, 0, 10, 10);
          const status2 = await swAsk({ t: "sogen-cache-status" });
          record(
            "evicted block refills correctly",
            bytesEqual(body, expected) && status2.blocks >= status1.blocks,
            `before=${status1.blocks} after=${status2.blocks}`,
          );
        } catch (error) {
          record("evicted block refills correctly", false, String(error));
        }

        // --- truncated / empty block: corrupt Cache Storage directly, must refuse and refill --------
        for (const [label, corruptLength] of [
          ["truncated block", 100],
          ["empty block", 0],
        ]) {
          try {
            const identity = await (
              await fetch(mainUrl, { method: "HEAD" })
            ).headers;
            const etag = identity.get("ETag") || identity.get("Last-Modified");
            const key = blockKey(
              origin,
              new URL(mainUrl).pathname,
              identity.get("X-Sogen-File-Id") || "",
              etag,
              fileSize,
              1,
            );

            const cache = await caches.open(CACHE_NAME);
            await cache.put(
              key,
              new Response(new Uint8Array(corruptLength), {
                headers: { "Content-Length": String(corruptLength) },
              }),
            );

            const { body } = await readRange(mainUrl, block + 500, 1000);
            const expected = expectedBytes(fileSize, 0, block + 500, 1000);
            record(
              label,
              bytesEqual(body, expected),
              `got ${body.length} bytes after corrupting block 1`,
            );
          } catch (error) {
            record(label, false, String(error));
          }
        }

        // --- cancelled prepare: only blocks [0, 1] warmed, none of the wrong length ------------------
        try {
          await caches.delete(CACHE_NAME);
          const warm = await swAsk({
            t: "sogen-cache-warm",
            url: mainUrl,
            from: 0,
            to: 1,
          });
          if (warm.error) throw new Error(warm.error);

          const cache = await caches.open(CACHE_NAME);
          const keys = await cache.keys();
          const blockKeys = keys.filter((k) =>
            k.url.includes("/__sogen-block/"),
          );

          let allCorrectLength = true;
          for (const k of blockKeys) {
            const response = await cache.match(k);
            const body = new Uint8Array(await response.clone().arrayBuffer());
            const index = Number(k.url.split("/").pop());
            const expectedLength = Math.min(block, fileSize - index * block);
            if (body.length !== expectedLength) allCorrectLength = false;
          }

          const noBlockPastCancel = !blockKeys.some(
            (k) => Number(k.url.split("/").pop()) > 1,
          );

          // The "cancel" itself is the page simply not asking for more blocks; the rest of the file
          // must still read correctly by falling back to the network per read.
          const { body } = await readRange(mainUrl, fileSize - 100, 100);
          const expected = expectedBytes(fileSize, 0, fileSize - 100, 100);

          record(
            "cancelled prepare leaves no block of the wrong length",
            allCorrectLength && noBlockPastCancel && bytesEqual(body, expected),
            `blocks cached: ${blockKeys.length}, all correct length: ${allCorrectLength}`,
          );
        } catch (error) {
          record(
            "cancelled prepare leaves no block of the wrong length",
            false,
            String(error),
          );
        }

        // --- two files, same mtime, different sizes: no key collision -------------------------------
        try {
          const aUrl = `${origin}/macos-root/collide-a.bin`;
          const bUrl = `${origin}/macos-root/collide-b.bin`;

          const { body: bodyA } = await readRange(aUrl, 0, 100);
          const { body: bodyB } = await readRange(bUrl, 0, 100);

          const expectedA = expectedBytes(block + 111, 5, 0, 100);
          const expectedB = expectedBytes(block + 222, 91, 0, 100);

          record(
            "same mtime, different sizes: no key collision",
            bytesEqual(bodyA, expectedA) && bytesEqual(bodyB, expectedB),
            `a matches=${bytesEqual(bodyA, expectedA)} b matches=${bytesEqual(bodyB, expectedB)}`,
          );
        } catch (error) {
          record(
            "same mtime, different sizes: no key collision",
            false,
            String(error),
          );
        }

        // --- two files, one validator, one size: no key collision -----------------------------------
        try {
          const aUrl = `${origin}/macos-root/twin-a.bin`;
          const bUrl = `${origin}/macos-root/twin-b.bin`;

          const { body: bodyA } = await readRange(aUrl, 0, twinSize);
          const { body: bodyB } = await readRange(bUrl, 0, twinSize);

          const expectedA = expectedBytes(twinSize, 17, 0, twinSize);
          const expectedB = expectedBytes(twinSize, 200, 0, twinSize);

          record(
            "one validator over two files: no key collision",
            bytesEqual(bodyA, expectedA) && bytesEqual(bodyB, expectedB),
            `a matches=${bytesEqual(bodyA, expectedA)} b matches=${bytesEqual(bodyB, expectedB)}`,
          );
        } catch (error) {
          record(
            "one validator over two files: no key collision",
            false,
            String(error),
          );
        }

        return out;
      },
      { block: BLOCK, fileSize: FILE_SIZE, twinSize: TWIN_SIZE },
    );

    results.push(...suiteResults);
  } finally {
    await context.close();
    await new Promise((resolve) => server.close(resolve));
  }

  if (consoleErrors.length) {
    results.push({
      name: "no uncaught page errors",
      pass: false,
      detail: consoleErrors.join("; "),
    });
  }

  return results;
}

function report(label, results) {
  console.log(`\n== ${label} ==`);
  let allPass = true;
  for (const { name, pass, detail, skipped } of results) {
    const tag = skipped ? "SKIP" : pass ? "PASS" : "FAIL";
    console.log(`  [${tag}] ${name}${detail ? ` (${detail})` : ""}`);
    if (!skipped && !pass) allPass = false;
  }
  return allPass;
}

async function main() {
  const fixedSource = await readFile(SW_PATH, "utf8");
  const pageModules = await transpilePageModules();

  // Two reverts, because the key has been wrong twice for different reasons and each fix needs its own
  // negative control. Dropping the URL scope leaves the pre-fix key, which two files behind one
  // validator collide on; dropping the size as well leaves the original key, which two files behind one
  // Last-Modified collide on. A revert that changes nothing means the control is not exercising
  // anything, so each one is checked to have actually matched.
  const KEY_LINE = "  return `${scope}|${identity.etag}:${identity.size}`;";

  function revert(replacement, what) {
    const source = fixedSource.replace(KEY_LINE, replacement);
    if (source === fixedSource) {
      throw new Error(
        `could not locate the blockKey return to revert to ${what} for the regression check`,
      );
    }
    return source;
  }

  const unscopedSource = revert(
    "  return `${identity.etag}:${identity.size}`;",
    "the validator and size",
  );
  const unfixedSource = revert(
    "  return `${identity.etag}`;",
    "the validator alone",
  );

  // No executablePath and no channel: playwright-core resolves an already-installed browser through
  // its own default registry lookup (the same one `npx playwright install` populates), so this runs
  // against whatever is on the machine rather than one path hardcoded for a single developer's setup.
  let browser;
  try {
    browser = await chromium.launch({ headless: true });
  } catch (error) {
    console.log(
      "SKIPPED: no compatible Chromium is installed for playwright-core " +
        `(${error && error.message ? error.message.split("\n")[0] : error}). ` +
        "Run `npx playwright install chromium` and re-run `npm run test:root-cache`.",
    );
    return;
  }

  try {
    const mustFail = (results, name, why) => {
      const row = results.find((r) => r.name === name);
      if (!row || row.pass) {
        throw new Error(
          `expected "${name}" to FAIL ${why}, but it did not -- the regression test is not ` +
            "actually exercising the bug",
        );
      }
      return row;
    };

    const unscopedResults = await runSuite(
      unscopedSource,
      browser,
      pageModules,
    );
    report(
      "WITHOUT the URL scope (expect one validator over two files to FAIL)",
      unscopedResults,
    );
    mustFail(
      unscopedResults,
      "one validator over two files: no key collision",
      "with the URL scope removed from the key",
    );

    const unfixedResults = await runSuite(unfixedSource, browser, pageModules);
    const unfixedOk = report(
      "WITHOUT the size-in-key fix (expect the collision case to FAIL)",
      unfixedResults,
    );

    const collisionRow = mustFail(
      unfixedResults,
      "same mtime, different sizes: no key collision",
      "without the Step 2 fix",
    );

    const otherRowsOk = unfixedResults
      .filter((r) => r.name !== "same mtime, different sizes: no key collision")
      .every((r) => r.pass);
    if (!otherRowsOk) {
      console.log(
        "\nNote: some unrelated cases also failed without the fix; only the collision case's failure is required.",
      );
    }

    const fixedResults = await runSuite(fixedSource, browser, pageModules);
    const fixedOk = report(
      "WITH the size-in-key fix (expect everything to PASS)",
      fixedResults,
    );

    console.log(
      `\nRegression check: collision case fails without the fix: ${!collisionRow.pass ? "confirmed" : "NOT confirmed"}`,
    );

    const distOk = report(
      "The built app (expect one service worker at / and a live precache)",
      await testBuiltApp(browser),
    );

    if (!fixedOk) {
      throw new Error("the fixed service worker did not pass every case");
    }

    if (!distOk) {
      throw new Error("the built app did not pass every case");
    }

    console.log(
      "\nAll adversarial cases pass with the committed service worker.",
    );
    void unfixedOk;
  } finally {
    await browser.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
