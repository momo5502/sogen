import { registerSW } from "virtual:pwa-register";
import Loader from "./Loader";

async function registerWorker() {
  await registerSW({
    immediate: true,
    /*onNeedRefresh() {
      Loader.setLoading(false);
      window.location.reload();
    },*/
    onOfflineReady() {
      Loader.setLoading(false);
    },
    onRegisteredSW(_, registration) {
      registration?.addEventListener("updatefound", () => {
        Loader.setLoading(true);
      });

      navigator.serviceWorker.addEventListener("controllerchange", () => {
        Loader.setLoading(false);
        window.location.reload();
      });
    },
  });
}

async function hasBeenRegisteredPreviously() {
  if (!("serviceWorker" in navigator)) {
    return false;
  }

  return (await navigator.serviceWorker.getRegistration()) !== undefined;
}

class Installer {
  private setupDone: boolean = false;
  private wasInstalled: boolean = false;
  private wasReinstalled: boolean = false;

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

  public async install() {
    if (this.wasReinstalled) {
      return;
    }

    this.wasInstalled = true;
    this.wasReinstalled = true;
    this.setupDone = true;
    await registerWorker();
  }

  public async reinstall() {
    if (this.isInstalled()) {
      await this.install();
    }
  }

  public async setup(awaitReinstall: boolean = false) {
    if (this.setupDone) {
      return;
    }

    this.wasInstalled = await hasBeenRegisteredPreviously();
    this.setupDone = true;

    const reinstallPromise = this.reinstall();

    if (awaitReinstall) {
      await reinstallPromise;
    }
  }
}

export default new Installer();
