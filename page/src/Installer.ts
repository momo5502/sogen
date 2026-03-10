import { registerSW } from "virtual:pwa-register";
import Loader from "./Loader";

function registerWorker() {
  console.log("Registering worker");
  registerSW({
    onNeedRefresh() {
      Loader.setLoading(false);
      window.location.reload();
    },
    onOfflineReady() {
      Loader.setLoading(false);
    },
    onRegisteredSW(_, registration) {
      registration?.addEventListener("updatefound", () => {
        Loader.setLoading(true);
      });
    },
  });
}

class Installer {
  private wasInstalled: boolean = false;

  public isInstalled(): boolean {
    return localStorage.getItem("pwa-installed") === "true";
  }

  public isRejected(): boolean {
    return localStorage.getItem("pwa-never-install") === "true";
  }

  public reject() {
    localStorage.setItem("pwa-never-install", "true");
  }

  public install() {
    if (this.wasInstalled) {
      console.log("Already installed");
      return;
    }

    console.log("Installing");

    this.wasInstalled = true;
    localStorage.setItem("pwa-installed", "true");
    registerWorker();
  }

  public reinstall() {
    if (this.isInstalled()) {
      this.install();
    }
  }
}

export default new Installer();
