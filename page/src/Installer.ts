import { registerSW } from "virtual:pwa-register";
import Loader from "./Loader";

let refreshing = false;
let controlled = false;

function watchForTakeover() {
  controlled = !!navigator.serviceWorker.controller;

  navigator.serviceWorker.addEventListener("controllerchange", () => {
    // The first change is this worker claiming a page that loaded uncontrolled; there is nothing to
    // pick up by reloading, and reloading every visitor once on their first visit is worse than the
    // race it would close. A later one is a genuine takeover and the page is now stale.
    if (!controlled) {
      controlled = true;
      return;
    }

    // An unguarded reload here turns any second worker claiming the scope into an unbounded navigation
    // loop -- measured at roughly one navigation per second while macos-root-sw.js was its own
    // registration at "/".
    if (refreshing) {
      return;
    }

    refreshing = true;
    Loader.setLoading(false);
    window.location.reload();
  });
}

// registerSW() reports the outcome through callbacks and swallows a failed register(), so the promise
// it hands back says nothing about whether a worker exists. Callers that depend on one -- macOS mode's
// root cache does -- need the failure, not a wait that never ends.
function registerWorker(): Promise<void> {
  if (!("serviceWorker" in navigator)) {
    return Promise.reject(
      new Error("service workers are unavailable in this browser"),
    );
  }

  return new Promise<void>((resolve, reject) => {
    registerSW({
      onOfflineReady() {
        Loader.setLoading(false);
      },
      onRegisterError(error: unknown) {
        reject(error instanceof Error ? error : new Error(String(error)));
      },
      onRegisteredSW(_, registration) {
        registration?.addEventListener("updatefound", () => {
          Loader.setLoading(true);
        });

        watchForTakeover();
        resolve();
      },
    });
  });
}

async function hasBeenRegisteredPreviously() {
  if (!("serviceWorker" in navigator)) {
    return false;
  }

  const registration = await navigator.serviceWorker.getRegistration();
  const script =
    registration?.active?.scriptURL ??
    registration?.waiting?.scriptURL ??
    registration?.installing?.scriptURL;

  return script !== undefined && new URL(script).pathname === "/sw.js";
}

class Installer {
  private setupDone: boolean = false;
  private wasInstalled: boolean = false;
  private registration: Promise<void> | null = null;

  public isInstalled() {
    if (!this.setupDone) {
      throw new Error("Need to setup first");
    }
    return this.wasInstalled;
  }

  public isRejected() {
    return localStorage.getItem("pwa-never-install") === "true";
  }

  public reject() {
    localStorage.setItem("pwa-never-install", "true");
  }

  // Registering is not the same act as installing. macOS mode reads its emulation root through the
  // worker's fetch handler, and giving that mode a worker of its own at "/" evicted this one rather
  // than joining it -- so that mode needs a registration whether or not the visitor ever asked for a
  // PWA, and must not be recorded as having asked. Awaited, so a caller that goes on to wait for a
  // controller learns about a failed register() instead of waiting for one that is never coming.
  public ensureServiceWorker(): Promise<void> {
    if (!this.registration) {
      const attempt = registerWorker();
      attempt.catch(() => {
        this.registration = null;
      });

      this.registration = attempt;
    }

    return this.registration;
  }

  public install(): Promise<void> {
    this.wasInstalled = true;
    this.setupDone = true;
    return this.ensureServiceWorker();
  }

  public async setup() {
    if (this.setupDone) {
      return;
    }

    this.wasInstalled = await hasBeenRegisteredPreviously();
    this.setupDone = true;

    if (this.wasInstalled) {
      this.ensureServiceWorker().catch(() => {});
    }
  }
}

export default new Installer();
