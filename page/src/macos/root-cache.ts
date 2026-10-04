// The page's view of page/public/macos-root-sw.js: reports what it holds and drives Prepare/Clear.
// The worker itself is not registered here -- vite.config.ts folds it into the one Workbox service
// worker at "/", because only one registration can exist per scope and a second one there evicts the
// precache every other mode depends on. Consumes nothing from earlier tasks -- it discovers the
// well-known layout of a served macOS root itself, the same way src/macos-web/app.js's
// attachMacosRoot() does, so a caller only needs the four functions below and never has to hand this
// module a list of URLs.

import Installer from "../Installer";
import { headSize, listServed } from "./served-listing";

export interface RootCacheState {
  enabled: boolean;
  bypassed: boolean;
  blocks: number;
  bytes: number;
  attachedBytes: number;
  persistent: boolean;
  quotaBytes: number;
  usageBytes: number;
  evictedBlocks: number;
}

export interface RootFileEntry {
  guestPath: string;
  url: string;
  size: number;
}

const SERVED_ROOT = "/macos-root";
const BLOCK_SIZE = 2 * 1024 * 1024;
const PREPARED_BLOCKS_KEY = "sogen-macos-root-cache-prepared-blocks";
const CACHE_WARM_WINDOW = 64;

const emptyState: RootCacheState = {
  enabled: false,
  bypassed: false,
  blocks: 0,
  bytes: 0,
  attachedBytes: 0,
  persistent: false,
  quotaBytes: 0,
  usageBytes: 0,
  evictedBlocks: 0,
};

let messageListenerInstalled = false;
let replyId = 0;
const pendingReplies = new Map<
  number,
  { resolve: (value: any) => void; reject: (reason: Error) => void } // eslint-disable-line @typescript-eslint/no-explicit-any
>();

function ensureMessageListener() {
  if (messageListenerInstalled) {
    return;
  }
  messageListenerInstalled = true;

  navigator.serviceWorker.addEventListener("message", (event) => {
    const id = event.data && event.data.id;
    const pending = pendingReplies.get(id);
    if (!pending) {
      return;
    }
    pendingReplies.delete(id);
    pending.resolve(event.data);
  });
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function swAsk(message: Record<string, unknown>): Promise<any> {
  const controller = navigator.serviceWorker.controller;
  if (!controller) {
    return Promise.reject(
      new Error("no service worker is controlling this page"),
    );
  }

  ensureMessageListener();

  const id = ++replyId;
  return new Promise((resolve, reject) => {
    pendingReplies.set(id, { resolve, reject });
    controller.postMessage({ ...message, id });
    setTimeout(() => {
      if (pendingReplies.delete(id)) {
        reject(new Error("the service worker did not answer"));
      }
    }, 600000);
  });
}

let discovered: RootFileEntry[] | null = null;
let discovering: Promise<RootFileEntry[]> | null = null;

// Mirrors attachMacosRoot() in src/macos-web/app.js: arm64e only (the same directory holds an x86_64 set
// an arm64 guest never reads), the dylinker itself, and the OS version plist CoreFoundation reads for the
// system version -- optional, since a root without it still runs anything that never reaches
// CoreFoundation. Memoized for the page's lifetime; discoverRootFiles's whole point is to answer this
// without the caller tracking what was attached.
async function discoverUncached(): Promise<RootFileEntry[]> {
  const entries: RootFileEntry[] = [];
  const origin = location.origin;

  const dyldUrl = `${origin}${SERVED_ROOT}/usr/lib/dyld`;
  entries.push({
    guestPath: "/usr/lib/dyld",
    url: dyldUrl,
    size: await headSize(dyldUrl),
  });

  const dyldDirUrl = `${origin}${SERVED_ROOT}/System/Library/dyld`;
  const listing = await listServed(dyldDirUrl);
  const cacheNames = listing
    .filter(
      (entry) =>
        !entry.directory && entry.name.startsWith("dyld_shared_cache_arm64e"),
    )
    .map((entry) => entry.name);

  for (const name of cacheNames) {
    const url = `${dyldDirUrl}/${encodeURIComponent(name)}`;
    entries.push({
      guestPath: `/System/Library/dyld/${name}`,
      url,
      size: await headSize(url),
    });
  }

  try {
    const versionUrl = `${origin}${SERVED_ROOT}/System/Library/CoreServices/SystemVersion.plist`;
    entries.push({
      guestPath: "/System/Library/CoreServices/SystemVersion.plist",
      url: versionUrl,
      size: await headSize(versionUrl),
    });
  } catch {
    /* absent is not fatal */
  }

  return entries;
}

export async function discoverRootFiles(): Promise<RootFileEntry[]> {
  if (discovered) {
    return discovered;
  }
  if (!discovering) {
    discovering = discoverUncached()
      .then((entries) => {
        discovered = entries;
        discovering = null;
        return entries;
      })
      .catch((error) => {
        discovering = null;
        throw error;
      });
  }
  return discovering;
}

// A run leaves the shared cache reachable at a different path than the one it was discovered under
// (dyld on a cryptex system opens it again under /System/Cryptexes/...); rediscovering is cheap enough
// (a handful of HEAD requests) that forcing it after Clear is simpler than trying to invalidate in place.
function resetDiscoveryCache() {
  discovered = null;
  discovering = null;
}

async function statusOrEmpty(): Promise<{
  blocks: number;
  bytes: number;
  block: number;
  bypassed: boolean;
} | null> {
  try {
    return await swAsk({ t: "sogen-cache-status" });
  } catch {
    return null;
  }
}

export async function rootCacheState(): Promise<RootCacheState> {
  if (!("serviceWorker" in navigator) || !navigator.serviceWorker.controller) {
    return { ...emptyState };
  }

  const status = await statusOrEmpty();
  if (!status) {
    return { ...emptyState, enabled: true };
  }

  let attachedBytes = 0;
  try {
    const files = await discoverRootFiles();
    attachedBytes = files.reduce((sum, file) => sum + file.size, 0);
  } catch {
    /* nothing attached yet, or the served root is unreachable -- report what the cache itself knows */
  }

  let persistent = false;
  try {
    persistent = (await navigator.storage?.persisted?.()) ?? false;
  } catch {
    /* not everywhere */
  }

  let quotaBytes = 0;
  let usageBytes = 0;
  try {
    const estimate = await navigator.storage?.estimate?.();
    quotaBytes = estimate?.quota ?? 0;
    usageBytes = estimate?.usage ?? 0;
  } catch {
    /* not everywhere */
  }

  const prepared = Number(localStorage.getItem(PREPARED_BLOCKS_KEY)) || 0;
  const evictedBlocks = Math.max(0, prepared - status.blocks);

  return {
    enabled: true,
    bypassed: !!status.bypassed,
    blocks: status.blocks,
    bytes: status.bytes,
    attachedBytes,
    persistent,
    quotaBytes,
    usageBytes,
    evictedBlocks,
  };
}

// Off does not tear down the registration -- that registration is also the Workbox worker precaching
// every other mode, so unregistering it the way src/macos-web/app.js's opt-root-cache does would take
// precaching down too. Instead the worker is told to stop intercepting /macos-root/ reads, so a run
// falls through to the same plain network Range requests it would make with no service worker at all.
export async function setRootCacheBypass(enabled: boolean): Promise<boolean> {
  const answer = await swAsk({ t: "sogen-cache-bypass", enabled });
  return !!answer.enabled;
}

let registration: Promise<RootCacheState> | null = null;

// Memoized on success, not on failure: every caller -- the panel's own mount effect and, critically,
// whatever starts a run -- needs the *same* controllerchange wait to have completed, not each its own
// separate one, and awaiting the one in-flight promise is what lets a run path block on it cheaply --
// near-instant on a warm visit, since the promise is already settled by then. But a transient
// register() failure (a hostile browser setting, a private window, a dropped connection) must not wedge
// every later caller behind one permanently-rejected promise forever -- the next call has to be free to
// try again. The rethrow after clearing is what still lets the awaiting caller see the failure; nothing
// here swallows it.
export function registerRootCache(): Promise<RootCacheState> {
  if (!registration) {
    registration = registerRootCacheUncached().catch((error) => {
      registration = null;
      throw error;
    });
  }
  return registration;
}

async function registerRootCacheUncached(): Promise<RootCacheState> {
  if (!("serviceWorker" in navigator)) {
    throw new Error("service workers are unavailable in this browser");
  }

  await Installer.ensureServiceWorker();

  // A worker the service worker does not control bypasses the cache entirely and silently. On a first
  // visit the page is not yet controlled, so wait for it before anything creates the emulator worker.
  if (!navigator.serviceWorker.controller) {
    await new Promise<void>((resolve) => {
      navigator.serviceWorker.addEventListener(
        "controllerchange",
        () => resolve(),
        { once: true },
      );
    });
  }

  try {
    await navigator.storage?.persist?.();
  } catch {
    /* a refusal is not a failure; the cache still works */
  }

  return rootCacheState();
}

let prepareCancelled = false;
let preparing = false;

export function isPreparingRoot(): boolean {
  return preparing;
}

export function cancelPrepareRoot(): void {
  prepareCancelled = true;
}

// Warmed in windows rather than whole files, so Cancel takes effect in a second or two rather than at
// the end of a multi-gigabyte subcache: nothing here can abort an in-flight sogen-cache-warm message, so
// the window size is what bounds how long one takes.
export async function prepareRoot(
  onProgress: (done: number, total: number) => void,
): Promise<void> {
  if (preparing) {
    return;
  }

  const targets = await discoverRootFiles();
  if (!targets.length) {
    return;
  }

  preparing = true;
  prepareCancelled = false;

  const total = targets.reduce((sum, target) => sum + target.size, 0);
  let done = 0;

  try {
    for (const target of targets) {
      if (prepareCancelled) {
        break;
      }

      const status = await swAsk({ t: "sogen-cache-status" });
      const blockSize = status.block || BLOCK_SIZE;
      const blocks = Math.max(1, Math.ceil(target.size / blockSize));

      for (let from = 0; from < blocks; from += CACHE_WARM_WINDOW) {
        if (prepareCancelled) {
          break;
        }

        const to = Math.min(from + CACHE_WARM_WINDOW - 1, blocks - 1);
        const answer = await swAsk({
          t: "sogen-cache-warm",
          url: target.url,
          from,
          to,
        });
        if (answer.error) {
          throw new Error(`${target.guestPath}: ${answer.error}`);
        }

        done += Math.min(
          (to - from + 1) * blockSize,
          Math.max(0, target.size - from * blockSize),
        );
        onProgress(done, total);
      }
    }
  } finally {
    preparing = false;

    // Only a Prepare that ran to the end sets the mark, or a cancelled one would make everything it
    // did not reach look like eviction ever after.
    if (!prepareCancelled) {
      try {
        const status = await swAsk({ t: "sogen-cache-status" });
        localStorage.setItem(PREPARED_BLOCKS_KEY, String(status.blocks || 0));
      } catch {
        /* the mark is a convenience, not a correctness thing */
      }
    }
  }
}

export async function clearRootCache(): Promise<void> {
  localStorage.removeItem(PREPARED_BLOCKS_KEY);
  resetDiscoveryCache();
  await swAsk({ t: "sogen-cache-clear" });
}
