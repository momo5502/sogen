"use strict";

// The emulation root is read by byte range from inside a guest memory fault: that bridge's XHR is
// synchronous and cannot await, so nothing on the read path can consult an asynchronous cache. A
// service worker can, because it sits at the network layer instead -- the synchronous XHR blocks on
// this fetch handler exactly as it already blocks on the network, and hostedRange needs no change.
//
// Cache Storage refuses to store a 206, so a range cannot be cached as it arrives. Every read is
// therefore snapped onto a fixed block grid, each whole block is stored as a plain 200 under a key
// naming the file's ETag and the block index, and the 206 the caller asked for is assembled out of
// those blocks. Getting that wrong returns the wrong bytes rather than failing, so the assembly is
// bounds-checked against the block's own recorded extent rather than against what was asked for.

const CACHE = "sogen-macos-root-v5";

// The chunk size the guest's pager reads in (MACOS_CACHE_CHUNK_SIZE), so a page-in is normally one
// block and never more than two.
const BLOCK = 2 * 1024 * 1024;

const ROOT = "/macos-root/";
const RANGE = /^bytes=(\d*)-(\d*)$/;

self.addEventListener("install", () => self.skipWaiting());

self.addEventListener("activate", (event) => {
  event.waitUntil(
    (async () => {
      for (const name of await caches.keys()) {
        if (name.startsWith("sogen-macos-root-") && name !== CACHE)
          await caches.delete(name);
      }
      // Without this the first visit runs uncontrolled and every read of it bypasses the cache, which
      // looks exactly like caching that does not work.
      await self.clients.claim();
    })(),
  );
});

const identities = new Map();

function parseContentRange(header) {
  const match = /^bytes (\d+)-(\d+)\/(\d+|\*)$/.exec((header || "").trim());
  if (!match) return null;
  return {
    start: Number(match[1]),
    end: Number(match[2]),
    total: match[3] === "*" ? 0 : Number(match[3]),
  };
}

// An emulation root is built out of symlinks, so the same shared cache is reachable at more than one
// path -- the page attaches /System/Library/dyld/..., and dyld on a cryptex system opens the same bytes
// at /System/Cryptexes/OS/System/Library/dyld/.... Keying on the URL cached them twice and meant
// preparing one path did nothing for the other: measured, a run after a full 5.45 GiB prepare still
// pulled 990 MiB. So blocks are shared between URLs -- but only when the server has said which file is
// behind each of them.
//
// Nothing in HTTP makes a validator unique across URLs; it distinguishes versions of one resource.
// Taking it for a file's identity anyway is what made two different files one. A validator of size and
// mtime is the size alone on a sealed macOS system volume, because every file there carries the same
// mtime: measured on a macOS 26 root, 2,325 of its 2,510 framework Info.plists shared one with a
// different framework's. Reading CoreUI's returned DeveloperToolsSupport's, at exactly the right length,
// so every short-read guard here passed and CFBundle went looking for
// CoreUI.framework/DeveloperToolsSupport.
//
// The validator still belongs in the key -- it is what makes a changed file miss -- and so does the
// size, because a server that sends no ETag at all leaves Last-Modified as the whole key.
function blockKey(identity) {
  const scope = identity.id ? `id=${identity.id}` : `at=${identity.path}`;
  return `${scope}|${identity.etag}:${identity.size}`;
}

function blockRequest(identity, index) {
  return new Request(
    `${self.location.origin}/__sogen-block/${encodeURIComponent(blockKey(identity))}/${index}`,
  );
}

// Size and validator for a root file, probed once per file per worker lifetime. A probe that cannot
// reach the network falls back to what a previous visit recorded, which is what makes a warmed root
// usable offline; a probe that succeeds and disagrees drops the stale blocks rather than mixing them.
async function identify(url, cache) {
  if (identities.has(url)) return identities.get(url);

  const recordKey = new Request(`${url}?__sogen_identity=1`);
  const recorded = await cache.match(recordKey);
  const previous = recorded ? await recorded.json() : null;

  let identity = previous;

  try {
    const head = await fetch(url, { method: "HEAD", cache: "no-store" });
    if (head.ok) {
      identity = {
        id: head.headers.get("X-Sogen-File-Id") || "",
        path: new URL(url).pathname,
        etag:
          head.headers.get("ETag") ||
          head.headers.get("Last-Modified") ||
          "none",
        size: Number(head.headers.get("Content-Length")) || 0,
        type: head.headers.get("Content-Type") || "application/octet-stream",
      };
    }
  } catch {
    /* offline: a recorded identity is better than refusing to serve */
  }

  if (!identity) return null;

  if (previous && blockKey(previous) !== blockKey(identity)) {
    const stale = `${self.location.origin}/__sogen-block/${encodeURIComponent(blockKey(previous))}/`;
    for (const key of await cache.keys()) {
      if (key.url.startsWith(stale)) await cache.delete(key);
    }
  }

  if (
    !previous ||
    previous.etag !== identity.etag ||
    previous.size !== identity.size
  ) {
    await cache.put(
      recordKey,
      new Response(JSON.stringify(identity), {
        headers: { "Content-Type": "application/json" },
      }),
    );
  }

  identities.set(url, identity);
  return identity;
}

// Contiguous runs are fetched as one request rather than one per block: a cold pre-warm of a 1.7 GB
// file is 850 blocks, and 850 round trips is a different thing from 850 blocks in a few requests.
async function fillBlocks(url, identity, first, last, cache) {
  let index = first;

  while (index <= last) {
    let end = index;
    while (
      end + 1 <= last &&
      !(await cache.match(blockRequest(identity, end + 1)))
    )
      ++end;

    const from = index * BLOCK;
    const to = Math.min((end + 1) * BLOCK, identity.size) - 1;

    if (to < from) return;

    const response = await fetch(url, {
      headers: { Range: `bytes=${from}-${to}` },
      cache: "no-store",
    });

    // Only a 206 says "this body is the range you asked for". A 200 is the whole entity, which is what
    // this server answers an If-Range miss with, and slicing it as though it began at `from` would file
    // the wrong bytes under the right key -- worse than storing nothing, because it is never noticed.
    if (response.status !== 206) return;

    const range = parseContentRange(response.headers.get("Content-Range"));
    if (!range || range.start !== from) return;

    const body = new Uint8Array(await response.arrayBuffer());
    const total = range.total || identity.size;

    for (let block = index; block <= end; ++block) {
      const offset = (block - index) * BLOCK;
      if (offset >= body.length) break;

      const slice = body.slice(offset, Math.min(offset + BLOCK, body.length));
      const blockStart = block * BLOCK;

      // A block is stored only when it is provably whole: a full block, or the file's genuine last one.
      // Anything else -- a truncated transfer, a server restarted mid-range, a Prepare cancelled -- would
      // be read back later as if it were complete and assembled into a short answer, and a short answer
      // to a Mach-O header parses as a segment at address zero.
      if (slice.length !== BLOCK && blockStart + slice.length !== total)
        continue;

      await cache.put(
        blockRequest(identity, block),
        new Response(slice, {
          headers: {
            "Content-Type": "application/octet-stream",
            "Content-Length": String(slice.length),
            "X-Sogen-Block-Start": String(blockStart),
          },
        }),
      );
    }

    index = end + 1;
  }
}

// A Cache Storage lookup is the expensive part of a hit -- measured at roughly 20 ms per 2 MiB read
// when every block was looked up twice, once to test for it and once to read it. One pass, and the
// bytes come out of the same Response that answered the test.
async function loadBlocks(url, identity, first, last, cache) {
  const blocks = new Array(last - first + 1).fill(null);
  let missingFrom = -1;
  let missingTo = -1;

  for (let index = first; index <= last; ++index) {
    const hit = await cache.match(blockRequest(identity, index));
    if (hit) {
      blocks[index - first] = hit;
    } else if (missingFrom < 0) {
      missingFrom = missingTo = index;
    } else {
      missingTo = index;
    }
  }

  if (missingFrom >= 0) {
    await fillBlocks(url, identity, missingFrom, missingTo, cache);

    for (let index = missingFrom; index <= missingTo; ++index) {
      if (!blocks[index - first])
        blocks[index - first] = await cache.match(
          blockRequest(identity, index),
        );
    }
  }

  return blocks;
}

async function rangeFromBlocks(url, identity, start, end, cache) {
  const first = Math.floor(start / BLOCK);
  const last = Math.floor(end / BLOCK);
  const blocks = await loadBlocks(url, identity, first, last, cache);

  if (blocks.some((block) => !block)) return null;

  const wanted = end - start + 1;
  const out = new Uint8Array(wanted);
  let written = 0;

  for (let index = first; index <= last; ++index) {
    const block = new Uint8Array(await blocks[index - first].arrayBuffer());
    const base = index * BLOCK;

    // Every block must be exactly the length its position implies. Trusting a short one is what turned
    // an evicted or truncated block into a successful empty read. Dropping it as well as refusing it
    // means the next read refills it instead of going to the network forever.
    if (block.length !== Math.min(BLOCK, identity.size - base)) {
      await cache.delete(blockRequest(identity, index));
      return null;
    }

    const from = Math.max(start, base) - base;
    const to = Math.min(end + 1, base + block.length) - base;
    if (to <= from) return null;

    out.set(block.subarray(from, to), written);
    written += to - from;
  }

  // Never a short answer. A caller that asked for a range and is handed fewer bytes with a 206 has no
  // way to tell that from a genuine short file, so anything less than the whole range goes to the
  // network instead.
  return written === wanted ? out : null;
}

async function serveRange(request, url, header) {
  const match = RANGE.exec(header.trim());
  if (!match) return fetch(request);

  const cache = await caches.open(CACHE);
  const identity = await identify(url, cache);
  if (!identity || !identity.size) return fetch(request);

  let start;
  let end;

  if (match[1] === "") {
    if (match[2] === "") return fetch(request);
    start = Math.max(0, identity.size - Number(match[2]));
    end = identity.size - 1;
  } else {
    start = Number(match[1]);
    end =
      match[2] === ""
        ? identity.size - 1
        : Math.min(Number(match[2]), identity.size - 1);
  }

  if (start > end || start >= identity.size) return fetch(request);

  // Length, not truthiness: a zero-length Uint8Array is truthy, and this guard reading `!body` is what
  // served an empty 206 for a Mach-O header. Checked here as well as inside rangeFromBlocks because
  // getting it wrong returns wrong bytes rather than an error.
  const wanted = end - start + 1;
  const body = await rangeFromBlocks(url, identity, start, end, cache);
  if (!body || body.length !== wanted) return fetch(request);

  return new Response(body, {
    status: 206,
    statusText: "Partial Content",
    headers: {
      "Content-Type": identity.type,
      "Content-Length": String(body.length),
      "Content-Range": `bytes ${start}-${start + body.length - 1}/${identity.size}`,
      "Accept-Ranges": "bytes",
      ETag: identity.etag,
      "X-Sogen-Cache": "block",
    },
  });
}

// "Off" for the root cache means the guest reads straight off the network with nothing intercepting --
// not tearing down this registration, which also holds the Workbox precache every other mode depends
// on. The flag lives in its own cache, not CACHE: sogen-cache-clear deletes CACHE wholesale, and a flag
// that shared it would be erased by Clear while the in-memory mirror below still answered "bypassed" --
// the next worker the browser recycles would then find nothing in Cache Storage and silently start
// caching again with the page still showing the box unchecked. Named outside the "sogen-macos-root-"
// family so the activate handler's version cleanup, which deletes every other name in that family, does
// not sweep it up either.
const SETTINGS_CACHE = "sogen-macos-settings";
const BYPASS_KEY = new Request(`${self.location.origin}/__sogen-bypass`);
let bypassKnown = null;

// The mirror is checked before anything async: every request on this path already pays for a fetch (or
// a Cache Storage read) beyond this, so a cache lookup purely to answer "is bypass on" on every single
// one of them is the exact cost this mirror exists to avoid. Only a cold worker -- bypassKnown still
// null -- pays it, once.
async function isBypassed() {
  if (bypassKnown !== null) return bypassKnown;
  const cache = await caches.open(SETTINGS_CACHE);
  bypassKnown = !!(await cache.match(BYPASS_KEY));
  return bypassKnown;
}

self.addEventListener("fetch", (event) => {
  const request = event.request;
  if (request.method !== "GET") return;

  const url = new URL(request.url);
  if (url.origin !== self.location.origin || !url.pathname.startsWith(ROOT))
    return;
  if (
    url.pathname.startsWith("/__sogen-block/") ||
    url.searchParams.has("__sogen_identity")
  )
    return;

  const header = request.headers.get("Range");
  if (!header) return;

  event.respondWith(
    (async () => {
      if (await isBypassed()) return fetch(request);
      return serveRange(request, url.origin + url.pathname, header).catch(() =>
        fetch(request),
      );
    })(),
  );
});

// The page asks what is cached and drives pre-warming through here rather than reaching into Cache
// Storage itself: only this worker knows the block grid and the key shape, and two implementations of
// that would drift.
self.addEventListener("message", (event) => {
  const data = event.data;
  if (!data || !data.t) return;

  const reply = (payload) =>
    event.source && event.source.postMessage({ id: data.id, ...payload });

  if (data.t === "sogen-cache-status") {
    event.waitUntil(
      (async () => {
        const cache = await caches.open(CACHE);
        const keys = await cache.keys();
        const versions = new Set();
        let blocks = 0;

        for (const key of keys) {
          const at = new URL(key.url);
          if (!at.pathname.startsWith("/__sogen-block/")) continue;
          ++blocks;
          versions.add(at.pathname.split("/")[2]);
        }

        reply({
          t: "status",
          blocks,
          bytes: blocks * BLOCK,
          block: BLOCK,
          files: versions.size,
          bypassed: await isBypassed(),
        });
      })(),
    );
    return;
  }

  if (data.t === "sogen-cache-bypass") {
    event.waitUntil(
      (async () => {
        const cache = await caches.open(SETTINGS_CACHE);
        if (data.enabled) {
          await cache.put(BYPASS_KEY, new Response("1"));
        } else {
          await cache.delete(BYPASS_KEY);
        }
        bypassKnown = !!data.enabled;
        reply({ t: "bypass", enabled: bypassKnown });
      })(),
    );
    return;
  }

  if (data.t === "sogen-cache-warm") {
    event.waitUntil(
      (async () => {
        const cache = await caches.open(CACHE);

        try {
          const identity = await identify(data.url, cache);
          if (!identity || !identity.size) {
            reply({
              t: "warmed",
              url: data.url,
              done: 0,
              total: 0,
              error: "no identity",
            });
            return;
          }

          const total = Math.ceil(identity.size / BLOCK);
          const from = Math.min(data.from || 0, total);
          const to = Math.min(
            data.to === undefined ? total - 1 : data.to,
            total - 1,
          );

          for (let index = from; index <= to; ++index) {
            if (!(await cache.match(blockRequest(identity, index)))) {
              await fillBlocks(
                data.url,
                identity,
                index,
                Math.min(index + 7, to),
                cache,
              );
              index = Math.min(index + 7, to);
            }
          }

          reply({
            t: "warmed",
            url: data.url,
            done: to - from + 1,
            total,
            size: identity.size,
          });
        } catch (error) {
          reply({
            t: "warmed",
            url: data.url,
            error: String(error && error.message ? error.message : error),
          });
        }
      })(),
    );
    return;
  }

  if (data.t === "sogen-cache-clear") {
    event.waitUntil(
      (async () => {
        await caches.delete(CACHE);
        identities.clear();
        reply({ t: "cleared" });
      })(),
    );
  }
});
